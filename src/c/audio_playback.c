#include "audio_playback.h"
#include "config.h"

// Hand the ring's contents to the speaker in small blocks rather than one giant
// write: speaker_stream_write() only takes what fits in the firmware's internal
// streaming buffer ("may be less if buffer is full") and the firmware faults
// outright on an oversized single write (the old 0x12112d45 crash). We feed
// SPEAKER_WRITE_CHUNK bytes at a time, respect the returned count, and come back
// via a short timer when the buffer is momentarily full or the ring runs dry.
#define SPEAKER_WRITE_CHUNK 512
#define SPEAKER_PUMP_INTERVAL_MS 20

// Re-grant credit to the phone only once roughly half the ring has freed up, so
// we send about one CHUNK_REQUEST per half-ring consumed instead of one per
// chunk (which would double the round trips and halve throughput).
#define GRANT_BATCH_CHUNKS ((AUDIO_RING_BYTES / AUDIO_CHUNK_SIZE) / 2)

static uint8_t *s_ring;
static uint32_t s_total;       // total bytes expected for this phrase
static uint32_t s_received;    // bytes written into the ring (monotonic)
static uint32_t s_played;      // bytes handed to the speaker (monotonic)
static uint32_t s_chunk_count; // ceil(s_total / AUDIO_CHUNK_SIZE)
static SpeakerPcmFormat s_format;
static bool s_speaker_open;    // speaker_stream_open() has been called
static bool s_playing;         // a playback session is active (ring allocated)
static int s_last_grant;       // highest chunk index granted to the phone so far
static AppTimer *s_pump_timer;
static AudioRequestCallback s_request_cb;
static AudioPlaybackDoneCallback s_done_cb;

// Tears down all session state and frees the ring. Does not touch the speaker
// (callers handle that) or invoke the done callback.
static void prv_reset_state(void) {
  if (s_pump_timer) {
    app_timer_cancel(s_pump_timer);
    s_pump_timer = NULL;
  }
  free(s_ring);
  s_ring = NULL;
  s_total = s_received = s_played = 0;
  s_chunk_count = 0;
  s_speaker_open = false;
  s_playing = false;
  s_last_grant = -1;
  s_request_cb = NULL;
  s_done_cb = NULL;
}

// Highest chunk index that still fits under (already-played bytes + ring
// capacity). Raising this as s_played grows is what lets the phone keep sending.
static void prv_recompute_grant(void) {
  if (!s_request_cb || s_chunk_count == 0) {
    return;
  }
  long grant = (long)((s_played + AUDIO_RING_BYTES) / AUDIO_CHUNK_SIZE) - 1;
  if (grant > (long)s_chunk_count - 1) {
    grant = (long)s_chunk_count - 1;
  }
  // Batch grants (half a ring at a time) except always release the final index
  // so the tail of a phrase isn't left ungranted.
  if (grant > s_last_grant &&
      (grant - s_last_grant >= (long)GRANT_BATCH_CHUNKS || grant == (long)s_chunk_count - 1)) {
    // Only advance s_last_grant if the request actually went out; a busy outbox
    // returns false and the next pump (20ms) retries the same grant.
    if (s_request_cb((int)grant)) {
      s_last_grant = (int)grant;
    }
  }
}

static void prv_finish_speaker_cb(SpeakerFinishReason reason, void *context) {
  bool success = (reason == SpeakerFinishReasonDone) && (s_played >= s_total);
  AudioPlaybackDoneCallback cb = s_done_cb;
  prv_reset_state();
  if (cb) {
    cb(success);
  }
}

// Writes as much of the ring as the speaker will currently accept, in
// SPEAKER_WRITE_CHUNK blocks that never cross the ring's wrap boundary in a
// single call. Reschedules itself while audio is still in flight (speaker full,
// or ring momentarily dry but more bytes are coming). Closes the stream once
// every byte has been handed off; the speaker's finish callback then fires when
// the audio actually finishes playing.
static void prv_pump(void *context) {
  s_pump_timer = NULL;
  if (!s_ring || !s_speaker_open) {
    return;
  }

  while (s_played < s_received) {
    uint32_t avail = s_received - s_played;
    uint32_t pos = s_played % AUDIO_RING_BYTES;
    uint32_t to_end = AUDIO_RING_BYTES - pos;
    uint32_t want = avail < SPEAKER_WRITE_CHUNK ? avail : SPEAKER_WRITE_CHUNK;
    if (want > to_end) {
      want = to_end;  // don't wrap past the ring's end in one write
    }
    uint32_t wrote = speaker_stream_write(s_ring + pos, want);
    s_played += wrote;
    if (wrote < want) {
      // Speaker's internal buffer is full -- let it drain, then continue.
      prv_recompute_grant();
      s_pump_timer = app_timer_register(SPEAKER_PUMP_INTERVAL_MS, prv_pump, NULL);
      return;
    }
  }

  if (s_played >= s_total) {
    // Every byte delivered. Close the stream and free the ring now; the finish
    // callback fires (and resets the rest) once the audio truly ends.
    speaker_stream_close();
    free(s_ring);
    s_ring = NULL;
    return;
  }

  // Our ring emptied before the phrase finished, but that's not necessarily an
  // audible gap: the speaker's firmware buffer still holds what we've handed it.
  // Keep the stream open and poll for the next chunk.
  prv_recompute_grant();
  s_pump_timer = app_timer_register(SPEAKER_PUMP_INTERVAL_MS, prv_pump, NULL);
}

bool audio_playback_begin(uint32_t total_len, int format,
                          AudioRequestCallback request_cb,
                          AudioPlaybackDoneCallback done_cb) {
  audio_playback_abort();

  if (total_len == 0) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "audio_playback: refusing total_len=0");
    return false;
  }
  s_ring = malloc(AUDIO_RING_BYTES);
  if (!s_ring) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "audio_playback: ring malloc(%d) failed (heap free %lu)",
            AUDIO_RING_BYTES, (unsigned long)heap_bytes_free());
    return false;
  }

  s_total = total_len;
  s_received = 0;
  s_played = 0;
  s_chunk_count = (total_len + AUDIO_CHUNK_SIZE - 1) / AUDIO_CHUNK_SIZE;
  s_format = (SpeakerPcmFormat)format;
  s_speaker_open = false;
  s_playing = true;
  s_last_grant = -1;
  s_request_cb = request_cb;
  s_done_cb = done_cb;

  // Release the first window of credit (as many chunks as the ring can hold).
  prv_recompute_grant();
  return true;
}

void audio_playback_append_chunk(uint32_t chunk_index, const uint8_t *data, uint32_t len) {
  if (!s_ring || !data || !s_playing) {
    return;
  }

  // Chunks arrive strictly in order; their bytes land right after the previous
  // ones. A mismatch means a dropped/reordered message -- bail rather than write
  // at the wrong offset (which would play as noise).
  uint64_t expected_offset = (uint64_t)chunk_index * AUDIO_CHUNK_SIZE;
  if (expected_offset != s_received) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "audio_playback: out-of-order chunk %lu (expected offset %lu, have %lu) -- ignoring",
            (unsigned long)chunk_index, (unsigned long)expected_offset,
            (unsigned long)s_received);
    return;
  }
  if (s_received - s_played + len > AUDIO_RING_BYTES) {
    APP_LOG(APP_LOG_LEVEL_ERROR,
            "audio_playback: ring overflow (credit bug) -- dropping chunk %lu",
            (unsigned long)chunk_index);
    return;
  }

  uint32_t pos = s_received % AUDIO_RING_BYTES;
  uint32_t to_end = AUDIO_RING_BYTES - pos;
  if (len <= to_end) {
    memcpy(s_ring + pos, data, len);
  } else {
    memcpy(s_ring + pos, data, to_end);             // up to the ring's end
    memcpy(s_ring, data + to_end, len - to_end);    // wrap the remainder
  }
  s_received += len;

  // Start the speaker once we've buffered a head start (or the whole, short
  // phrase has already arrived), then pump whatever's queued.
  bool just_opened = false;
  if (!s_speaker_open && (s_received >= AUDIO_PREBUFFER_BYTES || s_received >= s_total)) {
    if (speaker_get_status() != SpeakerStatusIdle) {
      speaker_stop();
    }
    uint8_t volume = (uint8_t)config_speech_volume();
    if (!speaker_stream_open(s_format, volume)) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "audio_playback: speaker_stream_open failed");
      AudioPlaybackDoneCallback cb = s_done_cb;
      prv_reset_state();
      if (cb) {
        cb(false);
      }
      return;
    }
    speaker_set_finish_callback(prv_finish_speaker_cb, NULL);
    s_speaker_open = true;
    just_opened = true;
  }

  if (s_speaker_open && (just_opened || !s_pump_timer)) {
    prv_pump(NULL);
  }
}

void audio_playback_abort(void) {
  if (s_speaker_open) {
    speaker_set_finish_callback(NULL, NULL);  // don't fire done_cb on our own stop
    speaker_stop();
  }
  prv_reset_state();
}
