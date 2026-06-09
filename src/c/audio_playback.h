#pragma once
#include <pebble.h>

// Must match CHUNK_SIZE in src/pkjs/index.js. The phone sends chunk N's bytes
// to land contiguously after chunk N-1 in the ring. Bumped 256 -> 1024:
// throughput is round-trip-latency bound (~45ms/ack), so fewer, larger chunks
// raise effective bytes/sec ~linearly. Must stay well under the inbox (2048)
// once the per-message dict overhead (~30B) is added.
#define AUDIO_CHUNK_SIZE 1024

// Streamed playback uses a small FIXED ring buffer instead of one malloc sized
// to the whole phrase. That removes the old design's real ceiling: the heap is
// badly fragmented during playback (e.g. ~94KB free but no contiguous 60KB
// block), so a per-phrase malloc capped phrases at ~5-6s. A 12KB ring fits
// trivially and bounds memory no matter how long the phrase is.
#define AUDIO_RING_BYTES (12 * 1024)

// Bytes that must accumulate in the ring before the speaker starts, so a slow
// first few chunks (or BT jitter) don't starve the speaker right at the start.
#define AUDIO_PREBUFFER_BYTES (6 * 1024)

typedef void (*AudioPlaybackDoneCallback)(bool success);

// Flow control: tells the phone the highest chunk index the watch can currently
// accept (a credit). The watch raises this as it drains the ring, so delivery
// self-throttles to the playback rate and the ring never overflows. Returns
// false if the request couldn't be sent (outbox busy) so the caller can retry.
typedef bool (*AudioRequestCallback)(int up_to_index);

// Begins a streamed playback of `total_len` bytes in `format`. Allocates only a
// small ring (AUDIO_RING_BYTES), not the whole phrase. `request_cb` is called
// immediately with the initial credit and again as the ring drains; `done_cb`
// fires when playback finishes (true) or fails (false). Returns false -- with
// nothing allocated and no callback pending -- if `total_len` is zero or the
// ring allocation fails.
bool audio_playback_begin(uint32_t total_len, int format,
                          AudioRequestCallback request_cb,
                          AudioPlaybackDoneCallback done_cb);

// Feeds chunk `chunk_index` into the ring (its bytes land right after the
// previous chunk; the credit scheme guarantees the space). Starts the speaker
// once AUDIO_PREBUFFER_BYTES have arrived, pumps audio to it, and raises the
// phone's credit as space frees. Ignored if no playback is in progress.
void audio_playback_append_chunk(uint32_t chunk_index, const uint8_t *data, uint32_t len);

// Cancels any in-progress streamed playback and frees the ring. Does NOT invoke
// the done callback (used for app-level aborts, e.g. a new dictation or BACK).
void audio_playback_abort(void);
