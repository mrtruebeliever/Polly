#pragma once
#include <pebble.h>

// Drives both the on-screen state label and which idle/talk animation runs.
typedef enum {
  UI_STATE_IDLE,
  UI_STATE_LISTENING,
  UI_STATE_THINKING,
  UI_STATE_SPEAKING,
  UI_STATE_ERROR,
} AppUiState;

// Discrete illustration poses swapped onto the parrot's BitmapLayer.
typedef enum {
  PARROT_POSE_IDLE,
  PARROT_POSE_BLINK,
  PARROT_POSE_TILT,
  PARROT_POSE_SPEAK_1,
  PARROT_POSE_SPEAK_2,
  PARROT_POSE_FLAP,
} ParrotPose;

void parrot_window_push(void);
void parrot_window_destroy(void);

// Registers the single-click handler for BUTTON_ID_SELECT (parrot_window owns
// the window's click config provider; the caller decides whether/how to react,
// e.g. only starting dictation while in UI_STATE_IDLE).
void parrot_window_set_select_handler(void (*handler)(void));

// Single-click handlers for UP (ask-AI) and DOWN (preset-phrase menu). Same
// pattern as SELECT: parrot_window owns the click config; the caller decides
// how to react (e.g. only while UI_STATE_IDLE).
void parrot_window_set_up_handler(void (*handler)(void));
void parrot_window_set_down_handler(void (*handler)(void));

// Centralizes UI transitions: updates the state label, shows/hides the speech
// bubble appropriately, and starts/stops the matching idle/talk animation.
void parrot_window_set_state(AppUiState state);
AppUiState parrot_window_get_state(void);

// Swaps the parrot illustration frame. Called by parrot_anim's timers.
void parrot_window_set_pose(ParrotPose pose);

// Nudges the parrot illustration by (dx, dy) pixels from its resting position
// (used by parrot_anim for the step/hop/bob fidgets). (0, 0) restores rest.
void parrot_window_set_offset(int16_t dx, int16_t dy);

// Wing-flap needs a sprite not in the idle group; these swap the resident pose
// group to {IDLE, FLAP} for the duration of a flap, then back. Loading is
// idempotent. parrot_anim calls these around its flap sequence so the heap only
// ever holds 2 pose bitmaps during a flap instead of a 4th alongside the idle 3.
void parrot_window_load_flap_group(void);
void parrot_window_load_idle_group(void);

// Shows `text` in the speech bubble (used for the dictated transcript).
void parrot_window_show_transcript(const char *text);

// Shows `text` as a one-line status/error message in the speech bubble.
void parrot_window_show_message(const char *text);

void parrot_window_clear_bubble(void);
