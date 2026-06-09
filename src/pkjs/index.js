var Clay = require('pebble-clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig);
var keys = require('message_keys');

var SAMPLE_RATE_HZ = 8000;
var CHUNK_SIZE = 1024;           // must match AUDIO_CHUNK_SIZE in audio_playback.h
var AUDIO_FORMAT_8KHZ_8BIT = 0;  // SpeakerPcmFormat_8kHz_8bit, see audio_playback.c

// Ask-AI: the UP button dictates a question, we ask Google Gemini (free tier)
// for a short answer, then feed that answer through the same Google-TTS speak()
// path. gemini-2.5-flash-lite is on the current free tier (the 2.0 models have
// a free-tier limit of 0 now), is fast, and -- unlike 2.5-flash -- doesn't spend
// the token budget on "thinking" by default. We still pin thinkingBudget to 0
// below so the whole maxOutputTokens budget goes to the spoken answer.
var GEMINI_MODEL = 'gemini-2.5-flash-lite';
var GEMINI_URL = 'https://generativelanguage.googleapis.com/v1beta/models/' + GEMINI_MODEL + ':generateContent';
var AI_MAX_TOKENS = 200;
var AI_SYSTEM = 'You are Polly, a parrot voice assistant on a smartwatch. Answer in at most ' +
  '1-2 short spoken sentences. Plain text only -- no markdown, lists, code, or emoji. ' +
  'Be direct and friendly.';

// Codes sent in TTS_ERROR -- the watch shows the same message for all of them,
// these only matter for `pebble logs` diagnosis.
var TTS_ERR_NO_KEY = 1;
var TTS_ERR_REQUEST_FAILED = 2;
var TTS_ERR_BAD_RESPONSE = 3;
var TTS_ERR_STREAM_FAILED = 4;

function sendTtsError(code) {
  var msg = {};
  msg[keys.TTS_ERROR] = code;
  Pebble.sendAppMessage(msg);
}

// --- base64 (PebbleKit JS has no atob/Buffer) --------------------------------

var BASE64_LOOKUP = (function () {
  var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  var lookup = {};
  for (var i = 0; i < chars.length; i++) { lookup[chars.charAt(i)] = i; }
  return lookup;
})();

function base64Decode(b64) {
  var bytes = [];
  var buffer = 0, bits = 0;
  for (var i = 0; i < b64.length; i++) {
    var value = BASE64_LOOKUP[b64.charAt(i)];
    if (value === undefined) { continue; } // '=' padding / whitespace
    buffer = (buffer << 6) | value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push((buffer >> bits) & 0xff);
    }
  }
  return bytes;
}

// --- WAV / PCM shaping --------------------------------------------------------

// Google's LINEAR16 response is a WAV file. Scan its chunks for "data" rather
// than assume the textbook 44-byte header -- an "fmt " chunk with extra fields
// or a "LIST" chunk would shift it.
function stripWavHeader(bytes) {
  function chunkId(offset) {
    return String.fromCharCode(bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]);
  }
  if (bytes.length < 12 || chunkId(0) !== 'RIFF' || chunkId(8) !== 'WAVE') {
    return bytes; // not a WAV -- assume already-raw PCM
  }
  var pos = 12;
  while (pos + 8 <= bytes.length) {
    var id = chunkId(pos);
    var size = bytes[pos + 4] | (bytes[pos + 5] << 8) | (bytes[pos + 6] << 16) | (bytes[pos + 7] << 24);
    var dataStart = pos + 8;
    if (id === 'data') {
      return bytes.slice(dataStart, dataStart + size);
    }
    pos = dataStart + size + (size & 1); // chunks are word-aligned
  }
  return bytes.slice(44); // fallback: textbook header size
}

// LINEAR16 is always 16-bit signed little-endian PCM -- Google's API has no
// 8-bit option. Keeping just the high byte of each sample halves both the
// reassembly buffer (matching the ~64KB AUDIO_MAX_BUFFER_BYTES budget that
// audio_playback.h derived from 8kHz/8-bit) and the Bluetooth transfer, which
// is the real bottleneck for streaming audio over AppMessage.
//
// Dropping the low 8 bits is a 256x quantization: one output LSB spans 256 of
// the input's levels. Plain truncation makes the rounding error track the
// signal, which the ear hears as tonal buzz/ticking in quiet passages. We add
// TPDF (triangular) dither -- two independent uniform sources summed, peaking
// at +/-1 output LSB (= +/-256 input units) -- *before* quantizing. That
// decorrelates the error into smooth white noise instead of patterned
// distortion, and preserves sub-LSB detail statistically. Clamp afterwards:
// dither can push a near-full-scale sample past the signed 8-bit range.
function downsampleTo8Bit(pcm16) {
  var out = [];
  for (var i = 0; i + 1 < pcm16.length; i += 2) {
    var sample = pcm16[i] | (pcm16[i + 1] << 8);
    if (sample > 0x7fff) { sample -= 0x10000; } // two's complement -> signed
    var dither = (Math.random() + Math.random() - 1) * 256; // TPDF, +/-1 LSB peak
    var q = Math.floor((sample + dither) / 256);
    if (q > 127) { q = 127; } else if (q < -128) { q = -128; }
    out.push(q & 0xff);
  }
  return out;
}

// --- Outbound: streamed PCM ---------------------------------------------------

// Credit-based streaming: the watch holds only a small ring buffer and grants
// us a "highest chunk index you may send" (CHUNK_REQUEST) that it raises as it
// drains the ring. We push chunks (ACK-chained, so the inbox never overflows)
// up to the current grant, then wait. This throttles delivery to the playback
// rate and keeps watch memory bounded regardless of phrase length. Any failure
// aborts the whole transfer -- a gap in the PCM just plays as noise.
var streamState = null;

function pumpStream() {
  var s = streamState;
  if (!s || s.sending) {
    return;
  }
  if (s.sentUpTo > s.grantedMax || s.sentUpTo >= s.chunkCount) {
    if (s.sentUpTo >= s.chunkCount) {
      streamState = null;  // everything sent; the watch drives playback to the end
    }
    return;
  }
  var i = s.sentUpTo;
  var start = i * CHUNK_SIZE;
  var msg = {};
  msg[keys.AUDIO_CHUNK_INDEX] = i;
  msg[keys.AUDIO_CHUNK] = s.pcm.slice(start, start + CHUNK_SIZE);
  s.sending = true;
  Pebble.sendAppMessage(msg, function () {
    if (streamState !== s) { return; }  // aborted/superseded mid-send
    s.sending = false;
    s.sentUpTo = i + 1;
    pumpStream();
  }, function () {
    console.log('polly: audio chunk ' + i + ' send failed');
    streamState = null;
    sendTtsError(TTS_ERR_STREAM_FAILED);
  });
}

function streamAudio(pcmBytes) {
  var total = pcmBytes.length;
  streamState = {
    pcm: pcmBytes,
    total: total,
    chunkCount: Math.ceil(total / CHUNK_SIZE),
    sentUpTo: 0,      // next chunk index to send
    grantedMax: -1,   // highest index the watch has granted so far
    sending: false
  };
  var head = {};
  head[keys.AUDIO_FORMAT] = AUDIO_FORMAT_8KHZ_8BIT;
  head[keys.AUDIO_TOTAL_LEN] = total;
  // Announce the size, then wait for the watch's first CHUNK_REQUEST.
  Pebble.sendAppMessage(head, function () {}, function () {
    console.log('polly: audio header send failed');
    streamState = null;
    sendTtsError(TTS_ERR_STREAM_FAILED);
  });
}

// --- TTS call -----------------------------------------------------------------

// Google rejects the request with HTTP 400 if voice.languageCode doesn't match
// voice.name's own locale (e.g. name "nl-BE-Wavenet-C" needs languageCode
// "nl-BE", not "nl-NL"). The settings dropdown can't list every locale Google
// has voices for, so when a specific voice name is given, derive the language
// code from its prefix instead of trusting the separate dropdown value.
function languageCodeFromVoiceName(name) {
  var match = name.match(/^([a-zA-Z]{2,3}-[a-zA-Z]{2})/);
  return match ? match[1] : null;
}

function speak(text) {
  var apiKey = localStorage.getItem('TTS_API_KEY') || '';
  if (!apiKey) {
    console.log('polly: no TTS API key configured');
    sendTtsError(TTS_ERR_NO_KEY);
    return;
  }

  var voiceName = localStorage.getItem('VOICE_NAME') || '';
  var languageCode = (voiceName && languageCodeFromVoiceName(voiceName))
    || localStorage.getItem('VOICE_LANG') || 'en-US';
  var voice = { languageCode: languageCode };
  if (voiceName) { voice.name = voiceName; }

  var url = 'https://texttospeech.googleapis.com/v1/text:synthesize?key=' + encodeURIComponent(apiKey);
  var body = JSON.stringify({
    input: { text: text },
    voice: voice,
    audioConfig: { audioEncoding: 'LINEAR16', sampleRateHertz: SAMPLE_RATE_HZ }
  });

  var xhr = new XMLHttpRequest();
  xhr.open('POST', url);
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.timeout = 20000;
  xhr.onload = function () {
    if (xhr.status < 200 || xhr.status >= 300) {
      console.log('polly: TTS HTTP ' + xhr.status + ': ' + xhr.responseText);
      sendTtsError(TTS_ERR_REQUEST_FAILED);
      return;
    }
    var pcm = null;
    try {
      var audioContent = JSON.parse(xhr.responseText).audioContent;
      pcm = downsampleTo8Bit(stripWavHeader(base64Decode(audioContent)));
    } catch (e) {
      console.log('polly: TTS response parse error: ' + e);
    }
    if (!pcm || !pcm.length) {
      sendTtsError(TTS_ERR_BAD_RESPONSE);
      return;
    }
    streamAudio(pcm);
  };
  xhr.onerror = function () { console.log('polly: TTS request failed'); sendTtsError(TTS_ERR_REQUEST_FAILED); };
  xhr.ontimeout = function () { console.log('polly: TTS request timed out'); sendTtsError(TTS_ERR_REQUEST_FAILED); };
  xhr.send(body);
}

// --- Ask-AI call --------------------------------------------------------------

function sendAnswer(text) {
  var msg = {};
  msg[keys.ANSWER] = text;
  Pebble.sendAppMessage(msg);  // lets the watch show the answer in the bubble while it speaks
}

function askAI(question) {
  var apiKey = localStorage.getItem('GEMINI_API_KEY') || '';
  if (!apiKey) {
    console.log('polly: no Gemini API key configured');
    sendTtsError(TTS_ERR_NO_KEY);
    return;
  }

  var body = JSON.stringify({
    system_instruction: { parts: [{ text: AI_SYSTEM }] },
    contents: [{ role: 'user', parts: [{ text: question }] }],
    generationConfig: {
      maxOutputTokens: AI_MAX_TOKENS,
      temperature: 0.7,
      thinkingConfig: { thinkingBudget: 0 }   // no thinking -> budget goes to the answer
    }
  });

  var xhr = new XMLHttpRequest();
  // Key goes in the query string, same as the Google TTS call.
  xhr.open('POST', GEMINI_URL + '?key=' + encodeURIComponent(apiKey));
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.timeout = 25000;
  xhr.onload = function () {
    if (xhr.status < 200 || xhr.status >= 300) {
      console.log('polly: AI HTTP ' + xhr.status + ': ' + xhr.responseText);
      sendTtsError(TTS_ERR_REQUEST_FAILED);
      return;
    }
    var answer = null;
    try {
      var data = JSON.parse(xhr.responseText);
      var cand = data.candidates && data.candidates[0];
      var parts = cand && cand.content && cand.content.parts;
      if (parts) {
        answer = '';
        for (var i = 0; i < parts.length; i++) {
          if (parts[i].text) { answer += parts[i].text; }
        }
      }
    } catch (e) {
      console.log('polly: AI response parse error: ' + e);
    }
    if (!answer || !answer.length) {
      sendTtsError(TTS_ERR_BAD_RESPONSE);
      return;
    }
    answer = answer.trim();
    sendAnswer(answer);
    speak(answer);
  };
  xhr.onerror = function () { console.log('polly: AI request failed'); sendTtsError(TTS_ERR_REQUEST_FAILED); };
  xhr.ontimeout = function () { console.log('polly: AI request timed out'); sendTtsError(TTS_ERR_REQUEST_FAILED); };
  xhr.send(body);
}

// --- Lifecycle ----------------------------------------------------------------

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

// Watch -> phone: either a QUESTION (UP button -> ask AI, then speak the answer)
// or a TRANSCRIPT (SELECT dictation / preset phrase -> speak verbatim).
Pebble.addEventListener('appmessage', function (e) {
  var d = (e && e.payload) || {};
  if (typeof d.CHUNK_REQUEST === 'number') {
    // Flow-control credit from the watch: raise the high-water index and push.
    if (streamState && d.CHUNK_REQUEST > streamState.grantedMax) {
      streamState.grantedMax = d.CHUNK_REQUEST;
      pumpStream();
    }
  } else if (typeof d.QUESTION === 'string' && d.QUESTION.length) {
    askAI(d.QUESTION);
  } else if (typeof d.TRANSCRIPT === 'string' && d.TRANSCRIPT.length) {
    speak(d.TRANSCRIPT);
  }
});

// Clay settings saved: TTS_API_KEY / VOICE_LANG / VOICE_NAME are phone-only --
// the phone makes the TTS call, so they're kept in localStorage and stripped
// out (never reaching the watch) before the rest is forwarded.
Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) { return; }
  var s = clay.getSettings(e.response);

  localStorage.setItem('TTS_API_KEY', (s[keys.TTS_API_KEY] || '').toString().trim());
  localStorage.setItem('VOICE_LANG', (s[keys.VOICE_LANG] || '').toString().trim());
  localStorage.setItem('VOICE_NAME', (s[keys.VOICE_NAME] || '').toString().trim());
  localStorage.setItem('GEMINI_API_KEY', (s[keys.GEMINI_API_KEY] || '').toString().trim());
  delete s[keys.TTS_API_KEY];
  delete s[keys.VOICE_LANG];
  delete s[keys.VOICE_NAME];
  delete s[keys.GEMINI_API_KEY];

  Pebble.sendAppMessage(s, function () {}, function () {
    console.log('polly: settings send failed');
  });
});
