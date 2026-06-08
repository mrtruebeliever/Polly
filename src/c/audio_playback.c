#include "audio_playback.h"
#include "config.h"

// Hand the PCM to the speaker in small blocks rather than one giant write:
// speaker_stream_write() only takes what fits in the firmware's internal
// streaming buffer ("may be less if buffer is full") and the firmware faults
// outright on an oversized single write (this was the 0x12112d45 crash). We
// feed SPEAKER_WRITE_CHUNK bytes at a time, respecting the returned count, and
// come back via a short timer when the buffer is momentarily full.
#define SPEAKER_WRITE_CHUNK 512
#define SPEAKER_PUMP_INTERVAL_MS 20

static uint8_t *s_buf;
static uint32_t s_total;
static uint32_t s_received;
static uint32_t s_play_offset;   // bytes already handed to the speaker
static SpeakerPcmFormat s_format;
static AudioPlaybackDoneCallback s_done_cb;
static bool s_playing;
static AppTimer *s_pump_timer;

static void prv_free_buffer(void) {
  free(s_buf);
  s_buf = NULL;
  s_total = 0;
  s_received = 0;
  s_play_offset = 0;
}

static void prv_finish_speaker_cb(SpeakerFinishReason reason, void *context) {
  bool success = (reason == SpeakerFinishReasonDone);
  AudioPlaybackDoneCallback cb = s_done_cb;
  s_done_cb = NULL;
  s_playing = false;
  prv_free_buffer();  // no-op if already freed in audio_playback_finish (s_buf is NULL)
  if (cb) {
    cb(success);
  }
}

// Writes as much as the speaker will currently accept, SPEAKER_WRITE_CHUNK
// bytes at a time. If the internal buffer fills (a short write), it reschedules
// itself to deliver the rest as the speaker drains. Once every byte is handed
// off it closes the stream and frees our buffer (the speaker keeps its copy).
static void prv_pump(void *context) {
  s_pump_timer = NULL;
  if (!s_buf) {
    return;
  }
  while (s_play_offset < s_total) {
    uint32_t remaining = s_total - s_play_offset;
    uint32_t want = remaining < SPEAKER_WRITE_CHUNK ? remaining : SPEAKER_WRITE_CHUNK;
    uint32_t wrote = speaker_stream_write(s_buf + s_play_offset, want);
    s_play_offset += wrote;
    if (wrote < want) {
      // Buffer full for now -- let the speaker drain, then deliver the rest.
      s_pump_timer = app_timer_register(SPEAKER_PUMP_INTERVAL_MS, prv_pump, NULL);
      return;
    }
  }
  speaker_stream_close();
  prv_free_buffer();  // speaker has its own copy; finish callback will fire on completion
}

bool audio_playback_begin(uint32_t total_len, int format) {
  audio_playback_abort();

  if (total_len == 0 || total_len > AUDIO_MAX_BUFFER_BYTES) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "audio_playback: refusing total_len=%lu",
            (unsigned long)total_len);
    return false;
  }
  s_buf = malloc(total_len);
  if (!s_buf) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "audio_playback: malloc(%lu) failed",
            (unsigned long)total_len);
    return false;
  }
  s_total = total_len;
  s_received = 0;
  s_format = (SpeakerPcmFormat)format;
  return true;
}

void audio_playback_append_chunk(uint32_t chunk_index, const uint8_t *data, uint32_t len) {
  if (!s_buf || !data) {
    return;
  }
  uint64_t offset = (uint64_t)chunk_index * AUDIO_CHUNK_SIZE;
  if (offset + len > s_total) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "audio_playback: chunk %lu (len %lu) would overrun buffer (total %lu) -- dropped",
            (unsigned long)chunk_index, (unsigned long)len, (unsigned long)s_total);
    return;
  }
  memcpy(s_buf + offset, data, len);
  s_received += len;
}

bool audio_playback_finish(AudioPlaybackDoneCallback callback) {
  if (!s_buf || s_received != s_total) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "audio_playback: incomplete transfer (%lu/%lu bytes) -- aborting",
            (unsigned long)s_received, (unsigned long)s_total);
    prv_free_buffer();
    if (callback) {
      callback(false);
    }
    return false;
  }

  if (speaker_get_status() != SpeakerStatusIdle) {
    speaker_stop();
  }

  uint8_t volume = (uint8_t)config_speech_volume();
  if (!speaker_stream_open(s_format, volume)) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "audio_playback: speaker_stream_open failed");
    prv_free_buffer();
    if (callback) {
      callback(false);
    }
    return false;
  }

  s_done_cb = callback;
  s_playing = true;
  s_play_offset = 0;
  speaker_set_finish_callback(prv_finish_speaker_cb, NULL);

  // Feed the speaker in chunks (see SPEAKER_WRITE_CHUNK note above). prv_pump
  // closes the stream and frees the buffer once everything has been handed off;
  // the finish callback then fires when the audio actually finishes playing.
  prv_pump(NULL);
  return true;
}

void audio_playback_abort(void) {
  if (s_pump_timer) {
    app_timer_cancel(s_pump_timer);
    s_pump_timer = NULL;
  }
  if (s_playing) {
    speaker_stop();
    s_playing = false;
  }
  s_done_cb = NULL;
  prv_free_buffer();
}
