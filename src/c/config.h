#pragma once
#include <pebble.h>

// Quick-phrase presets (spoken from the DOWN-button menu).
#define CONFIG_PRESET_SLOTS    4
// Per phrase, incl. NUL. Kept in step with TRANSCRIPT_MAX_CHARS (200) so a
// preset can be as long as a dictated phrase -- streaming playback no longer
// caps phrase length, and 64 was silently truncating longer presets mid-word.
// Well within persist's 256-byte string limit.
#define CONFIG_PRESET_BUF_SIZE 201

// Loads persisted settings (or defaults) into memory. Call once at startup.
void config_load(void);

// AppMessage inbox handler for settings pushed from the phone (Clay).
// Persists changed values; parrot_anim reads them live on its next timer tick.
void config_inbox_received(DictionaryIterator *iter, void *context);

bool config_idle_anim_enabled(void);
int config_idle_anim_freq(void);        // 0-100, scales idle animation interval
int config_speech_volume(void);         // 0-100, passed to speaker_stream_open

// Quick phrases: count of non-empty presets, and the index-th non-empty phrase
// (compacted -- index 0..count-1, skips blank slots). Returns "" if out of range.
int config_preset_count(void);
const char *config_preset_phrase(int index);
