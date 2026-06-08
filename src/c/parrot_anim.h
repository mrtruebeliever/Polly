#pragma once
#include <pebble.h>
#include "parrot_window.h"

// Owns the AppTimers that drive idle poses (blink/tilt) and the talking-beak
// alternation, switching between them based on the current AppUiState.
void parrot_anim_init(void);
void parrot_anim_deinit(void);

// Called by parrot_window whenever the UI state changes; starts/stops the
// matching timer set (idle cycling vs. talk-frame alternation).
void parrot_anim_set_state(AppUiState state);

// Touching the parrot while idle triggers a brief excited wiggle + chirp,
// then returns to normal idle cycling. No-op outside UI_STATE_IDLE or while
// a reaction is already playing (parrot_window is responsible for the
// touch-position/state checks; this just owns the reaction itself).
void parrot_anim_trigger_happy(void);
