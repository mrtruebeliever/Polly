#include "parrot_anim.h"
#include "config.h"

#define IDLE_MIN_INTERVAL_MS 3000
#define IDLE_MAX_INTERVAL_MS 9000
#define BLINK_HOLD_MS 250
#define TILT_HOLD_MS 1000
#define TALK_FRAME_MS 150
#define HAPPY_FRAME_MS 130
#define HAPPY_FRAME_COUNT 6  // ~780ms wiggle: alternates TILT/BLINK three times

typedef enum {
  IDLE_PHASE_NEUTRAL,  // showing PARROT_POSE_IDLE, waiting to do something cute
  IDLE_PHASE_POSED,    // briefly showing blink/tilt, about to revert
} IdlePhase;

static AppTimer *s_idle_timer;
static AppTimer *s_talk_timer;
static AppTimer *s_happy_timer;
static IdlePhase s_idle_phase = IDLE_PHASE_NEUTRAL;
static bool s_talk_alt = false;
static bool s_happy_alt = false;
static int s_happy_frames_left = 0;
static AppUiState s_state = UI_STATE_IDLE;

// Higher IDLE_ANIM_FREQ (0-100) -> shorter waits between idle poses.
static uint32_t random_idle_interval(void) {
  int freq = config_idle_anim_freq();
  if (freq < 0) { freq = 0; }
  if (freq > 100) { freq = 100; }
  uint32_t range = IDLE_MAX_INTERVAL_MS - IDLE_MIN_INTERVAL_MS;
  uint32_t base = IDLE_MAX_INTERVAL_MS - (range * (uint32_t)freq) / 100;
  return base + (rand() % 1500);
}

static void idle_timer_cb(void *data);

static void schedule_idle_timer(uint32_t delay_ms) {
  s_idle_timer = app_timer_register(delay_ms, idle_timer_cb, NULL);
}

static void idle_timer_cb(void *data) {
  s_idle_timer = NULL;
  if (s_idle_phase == IDLE_PHASE_NEUTRAL) {
    bool do_tilt = (rand() % 10) < 3;  // ~30% tilt, ~70% blink
    parrot_window_set_pose(do_tilt ? PARROT_POSE_TILT : PARROT_POSE_BLINK);
    s_idle_phase = IDLE_PHASE_POSED;
    schedule_idle_timer(do_tilt ? TILT_HOLD_MS : BLINK_HOLD_MS);
  } else {
    parrot_window_set_pose(PARROT_POSE_IDLE);
    s_idle_phase = IDLE_PHASE_NEUTRAL;
    schedule_idle_timer(random_idle_interval());
  }
}

static void stop_idle_timer(void) {
  if (s_idle_timer) {
    app_timer_cancel(s_idle_timer);
    s_idle_timer = NULL;
  }
  s_idle_phase = IDLE_PHASE_NEUTRAL;
}

static void start_idle_timer(void) {
  stop_idle_timer();
  if (config_idle_anim_enabled()) {
    schedule_idle_timer(random_idle_interval());
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
  s_idle_phase = IDLE_PHASE_NEUTRAL;
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
