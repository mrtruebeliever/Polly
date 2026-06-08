#include "parrot_anim.h"
#include "config.h"

#define IDLE_MIN_INTERVAL_MS 3000
#define IDLE_MAX_INTERVAL_MS 9000
#define BLINK_HOLD_MS 250
#define TILT_HOLD_MS 1000
#define TALK_FRAME_MS 150
#define HAPPY_FRAME_MS 130
#define HAPPY_FRAME_COUNT 6  // ~780ms wiggle: alternates TILT/BLINK three times

// One keyframe of an idle "fidget": nudge the parrot to (dx, dy) px from rest,
// optionally swap to `pose` (KEEP_POSE = leave unchanged), hold `ms`, advance.
#define KEEP_POSE (-1)
typedef struct { int8_t dx, dy, pose; uint16_t ms; } Fidget;

// Blink/tilt: pure pose swaps (no movement). Step/hop: pure layer movement over
// the resting pose. Bob: a "look around" -- tilt the head while bobbing. Flap:
// alternate IDLE<->FLAP (the FLAP group must be loaded first; see idle_timer_cb).
// Every sequence ends back at rest (0,0, IDLE) so the player cleans up uniformly.
static const Fidget FR_BLINK[] = {{0, 0, PARROT_POSE_BLINK, BLINK_HOLD_MS}, {0, 0, PARROT_POSE_IDLE, 1}};
static const Fidget FR_TILT[]  = {{0, 0, PARROT_POSE_TILT, TILT_HOLD_MS}, {0, 0, PARROT_POSE_IDLE, 1}};
static const Fidget FR_STEP_R[] = {{6, 0, KEEP_POSE, 150}, {0, 0, KEEP_POSE, 110}, {6, 0, KEEP_POSE, 150}, {0, 0, KEEP_POSE, 1}};
static const Fidget FR_STEP_L[] = {{-6, 0, KEEP_POSE, 150}, {0, 0, KEEP_POSE, 110}, {-6, 0, KEEP_POSE, 150}, {0, 0, KEEP_POSE, 1}};
static const Fidget FR_HOP[]   = {{0, -8, KEEP_POSE, 110}, {0, 0, KEEP_POSE, 70}, {0, -5, KEEP_POSE, 90}, {0, 0, KEEP_POSE, 1}};
static const Fidget FR_BOB[]   = {{0, 3, PARROT_POSE_TILT, 160}, {0, 0, PARROT_POSE_TILT, 140}, {0, 3, PARROT_POSE_TILT, 160}, {0, 0, PARROT_POSE_IDLE, 1}};
static const Fidget FR_FLAP[]  = {{0, 0, PARROT_POSE_FLAP, 140}, {0, -3, PARROT_POSE_IDLE, 120}, {0, 0, PARROT_POSE_FLAP, 140},
                                  {0, -3, PARROT_POSE_IDLE, 120}, {0, 0, PARROT_POSE_FLAP, 140}, {0, 0, PARROT_POSE_IDLE, 1}};

static AppTimer *s_idle_timer;     // wait between fidgets
static AppTimer *s_fidget_timer;   // steps through one fidget's keyframes
static AppTimer *s_talk_timer;
static AppTimer *s_happy_timer;
static const Fidget *s_fidget;
static int s_fidget_len, s_fidget_i;
static bool s_in_flap;
static bool s_talk_alt = false;
static bool s_happy_alt = false;
static int s_happy_frames_left = 0;
static AppUiState s_state = UI_STATE_IDLE;

// Higher IDLE_ANIM_FREQ (0-100) -> shorter waits between idle fidgets.
static uint32_t random_idle_interval(void) {
  int freq = config_idle_anim_freq();
  if (freq < 0) { freq = 0; }
  if (freq > 100) { freq = 100; }
  uint32_t range = IDLE_MAX_INTERVAL_MS - IDLE_MIN_INTERVAL_MS;
  uint32_t base = IDLE_MAX_INTERVAL_MS - (range * (uint32_t)freq) / 100;
  return base + (rand() % 1500);
}

static void schedule_next_fidget(void);

static void fidget_step(void *data) {
  s_fidget_timer = NULL;
  if (s_fidget_i >= s_fidget_len) {              // sequence finished
    parrot_window_set_offset(0, 0);
    if (s_in_flap) {                             // restore the idle pose group
      parrot_window_load_idle_group();
      s_in_flap = false;
    }
    schedule_next_fidget();
    return;
  }
  const Fidget *f = &s_fidget[s_fidget_i++];
  parrot_window_set_offset(f->dx, f->dy);
  if (f->pose != KEEP_POSE) {
    parrot_window_set_pose((ParrotPose)f->pose);
  }
  s_fidget_timer = app_timer_register(f->ms, fidget_step, NULL);
}

static void play_fidget(const Fidget *frames, int len) {
  s_fidget = frames;
  s_fidget_len = len;
  s_fidget_i = 0;
  fidget_step(NULL);
}

static void idle_timer_cb(void *data) {
  s_idle_timer = NULL;
  int r = rand() % 100;
  if (r < 35) {
    play_fidget(FR_BLINK, ARRAY_LENGTH(FR_BLINK));
  } else if (r < 55) {
    play_fidget(FR_TILT, ARRAY_LENGTH(FR_TILT));
  } else if (r < 70) {
    play_fidget((rand() & 1) ? FR_STEP_R : FR_STEP_L, ARRAY_LENGTH(FR_STEP_R));
  } else if (r < 82) {
    play_fidget(FR_HOP, ARRAY_LENGTH(FR_HOP));
  } else if (r < 92) {
    play_fidget(FR_BOB, ARRAY_LENGTH(FR_BOB));
  } else {
    parrot_window_load_flap_group();   // swap in the FLAP sprite (2 bitmaps, heap-safe)
    s_in_flap = true;
    play_fidget(FR_FLAP, ARRAY_LENGTH(FR_FLAP));
  }
}

static void schedule_next_fidget(void) {
  s_idle_timer = app_timer_register(random_idle_interval(), idle_timer_cb, NULL);
}

static void stop_idle_timer(void) {
  if (s_idle_timer) {
    app_timer_cancel(s_idle_timer);
    s_idle_timer = NULL;
  }
  if (s_fidget_timer) {
    app_timer_cancel(s_fidget_timer);
    s_fidget_timer = NULL;
  }
  parrot_window_set_offset(0, 0);
  // The pose group is owned by parrot_window_set_state (which loads the right
  // group before this runs on a state change). Callers that stop while staying
  // idle (trigger_happy) reload the idle group themselves.
  s_in_flap = false;
}

static void start_idle_timer(void) {
  stop_idle_timer();
  if (config_idle_anim_enabled()) {
    schedule_next_fidget();
  }
}

// --- Talking-beak alternation ------------------------------------------------

static void talk_timer_cb(void *data) {
  s_talk_alt = !s_talk_alt;
  parrot_window_set_pose(s_talk_alt ? PARROT_POSE_SPEAK_1 : PARROT_POSE_SPEAK_2);
  s_talk_timer = app_timer_register(TALK_FRAME_MS, talk_timer_cb, NULL);
}

static void stop_talk_timer(void) {
  if (s_talk_timer) {
    app_timer_cancel(s_talk_timer);
    s_talk_timer = NULL;
  }
}

static void start_talk_timer(void) {
  stop_talk_timer();
  s_talk_alt = false;
  talk_timer_cb(NULL);
}

// --- Touch reaction: brief wiggle + chirp ------------------------------------

// Three quick rising notes -- a synthesized "tweet", not streamed audio, so it
// plays instantly without round-tripping to the phone (unlike the TTS path in
// audio_playback.c). velocity=0 means "use the volume passed to
// speaker_play_notes" rather than a per-note override.
static void play_chirp(void) {
  static const SpeakerNote chirp[] = {
    { .midi_note = 79, .waveform = SpeakerWaveformSine, .duration_ms = 90,  .velocity = 0 },
    { .midi_note = 84, .waveform = SpeakerWaveformSine, .duration_ms = 90,  .velocity = 0 },
    { .midi_note = 91, .waveform = SpeakerWaveformSine, .duration_ms = 140, .velocity = 0 },
  };
  speaker_play_notes(chirp, ARRAY_LENGTH(chirp), (uint8_t)config_speech_volume());
}

static void stop_happy_timer(void) {
  if (s_happy_timer) {
    app_timer_cancel(s_happy_timer);
    s_happy_timer = NULL;
  }
}

static void happy_timer_cb(void *data) {
  if (s_happy_frames_left <= 0) {
    s_happy_timer = NULL;
    parrot_window_set_pose(PARROT_POSE_IDLE);
    start_idle_timer();
    return;
  }
  s_happy_alt = !s_happy_alt;
  // Stick to idle-set poses (BLINK/TILT) -- parrot_window only keeps the pose
  // bitmaps for the *current* UI state loaded, and a touch reaction can only
  // fire while UI_STATE_IDLE, so SPEAK_1/2 wouldn't be resident here.
  parrot_window_set_pose(s_happy_alt ? PARROT_POSE_BLINK : PARROT_POSE_TILT);
  s_happy_frames_left--;
  s_happy_timer = app_timer_register(HAPPY_FRAME_MS, happy_timer_cb, NULL);
}

void parrot_anim_trigger_happy(void) {
  if (s_state != UI_STATE_IDLE || s_happy_timer) {
    return;  // only react while idle; ignore re-taps mid-reaction
  }
  stop_idle_timer();
  parrot_window_load_idle_group();  // a flap may have been mid-play; happy needs BLINK/TILT
  play_chirp();
  s_happy_alt = false;
  s_happy_frames_left = HAPPY_FRAME_COUNT;
  happy_timer_cb(NULL);
}

// --- Public API --------------------------------------------------------------

void parrot_anim_set_state(AppUiState state) {
  if (s_state == state) {
    return;
  }
  s_state = state;
  stop_happy_timer();  // a real state change always wins over a touch-reaction

  if (state == UI_STATE_SPEAKING) {
    stop_idle_timer();
    start_talk_timer();
    return;
  }

  stop_talk_timer();
  parrot_window_set_pose(PARROT_POSE_IDLE);

  if (state == UI_STATE_IDLE) {
    start_idle_timer();
  } else {
    // Listening/thinking/error: hold a neutral pose, no idle fidgeting.
    stop_idle_timer();
  }
}

void parrot_anim_init(void) {
  s_talk_alt = false;
  // Deliberately not a valid AppUiState: forces the first parrot_anim_set_state()
  // call (made right after this, with UI_STATE_IDLE) to actually start the timer
  // instead of being swallowed by the no-op "state unchanged" guard.
  s_state = (AppUiState)-1;
}

void parrot_anim_deinit(void) {
  stop_idle_timer();
  stop_talk_timer();
  stop_happy_timer();
}
