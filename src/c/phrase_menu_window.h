#pragma once
#include <pebble.h>

// Pushes a MenuLayer window listing the configured quick phrases. Selecting a
// row pops the window and calls `on_pick(phrase)` (the phrase points into the
// config's stable storage). BACK just pops. No-op if there are no phrases.
void phrase_menu_window_push(void (*on_pick)(const char *phrase));
