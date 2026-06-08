#pragma once
#include <pebble.h>

// Must match CHUNK_SIZE in src/pkjs/index.js -- chunk N lands at byte offset
// N * AUDIO_CHUNK_SIZE in the reassembly buffer.
#define AUDIO_CHUNK_SIZE 256

// Defensive upper bound on a single phrase's PCM size (~16s @ 8kHz/8-bit),
// well under emery's 128KB app heap (MAX_APP_MEMORY_SIZE) but generous beyond
// what TRANSCRIPT_MAX_CHARS should ever produce -- guards against a malformed
// or hostile AUDIO_TOTAL_LEN exhausting the heap via malloc().
#define AUDIO_MAX_BUFFER_BYTES (64 * 1024)

typedef void (*AudioPlaybackDoneCallback)(bool success);

// Allocates a fresh reassembly buffer sized `total_len` for PCM in `format`
// (a SpeakerPcmFormat value, sent over AppMessage as a plain int). Frees any
// previous (e.g. aborted) buffer first. Returns false -- with nothing left
// allocated -- if `total_len` is zero, exceeds AUDIO_MAX_BUFFER_BYTES, or the
// allocation itself fails.
bool audio_playback_begin(uint32_t total_len, int format);

// Copies `len` bytes into the buffer at offset `chunk_index * AUDIO_CHUNK_SIZE`.
// Silently ignored if no transfer is in progress or the write would overrun
// the buffer (a partial PCM buffer would just play back as noise, so any
// inconsistency here is meant to surface via the completeness check in
// audio_playback_finish() rather than a partial playback attempt).
void audio_playback_append_chunk(uint32_t chunk_index, const uint8_t *data, uint32_t len);

// Call once the phone signals AUDIO_DONE. Verifies every byte arrived, then
// plays the buffer back in one shot via speaker_stream_open/write/close and
// frees the reassembly buffer immediately (the speaker keeps its own copy).
// Returns true if playback started -- `callback` will then fire later with
// `success=true` on natural completion (SpeakerFinishReasonDone) or `false`
// otherwise. Returns false if playback could not start (incomplete transfer or
// a busy/erroring speaker), in which case `callback` has already been invoked
// with `false` synchronously. Freeing the buffer before returning lets the
// caller load the memory-hungry SPEAK pose bitmaps without overflowing the heap.
bool audio_playback_finish(AudioPlaybackDoneCallback callback);

// Cancels any in-progress transfer or playback and frees the buffer. Does NOT
// invoke the done callback (used for app-level aborts, e.g. a new dictation
// interrupting playback -- the caller already knows why it's aborting).
void audio_playback_abort(void);
