#pragma once
#include <pebble.h>

// Loads persisted settings (or defaults) into memory. Call once at startup.
void config_load(void);

// AppMessage inbox handler for settings pushed from the phone (Clay).
// Persists changed values; parrot_anim reads them live on its next timer tick.
void config_inbox_received(DictionaryIterator *iter, void *context);

bool config_idle_anim_enabled(void);
int config_idle_anim_freq(void);        // 0-100, scales idle animation interval
int config_speech_volume(void);         // 0-100, passed to speaker_stream_open
