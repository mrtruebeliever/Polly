#include "config.h"

// Persist storage keys (independent of AppMessage key numbering).
#define PERSIST_IDLE_ANIM_ENABLED 1
#define PERSIST_IDLE_ANIM_FREQ    2
#define PERSIST_SPEECH_VOLUME     3

#define DEFAULT_IDLE_ANIM_ENABLED true
#define DEFAULT_IDLE_ANIM_FREQ    50
#define DEFAULT_SPEECH_VOLUME     70

static bool s_idle_anim_enabled = DEFAULT_IDLE_ANIM_ENABLED;
static int s_idle_anim_freq = DEFAULT_IDLE_ANIM_FREQ;
static int s_speech_volume = DEFAULT_SPEECH_VOLUME;

static int clamp_pct(int v) {
  if (v < 0) { return 0; }
  if (v > 100) { return 100; }
  return v;
}

bool config_idle_anim_enabled(void) { return s_idle_anim_enabled; }
int config_idle_anim_freq(void) { return s_idle_anim_freq; }
int config_speech_volume(void) { return s_speech_volume; }

void config_load(void) {
  if (persist_exists(PERSIST_IDLE_ANIM_ENABLED)) {
    s_idle_anim_enabled = persist_read_bool(PERSIST_IDLE_ANIM_ENABLED);
  }
  if (persist_exists(PERSIST_IDLE_ANIM_FREQ)) {
    s_idle_anim_freq = clamp_pct(persist_read_int(PERSIST_IDLE_ANIM_FREQ));
  }
  if (persist_exists(PERSIST_SPEECH_VOLUME)) {
    s_speech_volume = clamp_pct(persist_read_int(PERSIST_SPEECH_VOLUME));
  }
}

// Note: VOICE_LANG / VOICE_NAME / TTS_API_KEY are phone-only settings -- Clay
// needs message keys for them to round-trip out of the config webview, but
// pkjs/index.js intercepts and keeps them in localStorage rather than ever
// forwarding them here (the phone makes the TTS call, so the watch never
// needs them). Nothing to read or persist on this side.
void config_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;

  // No change-notify hook needed: parrot_anim reads config_*() live on each
  // timer tick, so persisting the new values here is enough for them to apply.
  if ((t = dict_find(iter, MESSAGE_KEY_IDLE_ANIM_ENABLED))) {
    s_idle_anim_enabled = (t->value->int32 != 0);
    persist_write_bool(PERSIST_IDLE_ANIM_ENABLED, s_idle_anim_enabled);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_IDLE_ANIM_FREQ))) {
    s_idle_anim_freq = clamp_pct(t->value->int32);
    persist_write_int(PERSIST_IDLE_ANIM_FREQ, s_idle_anim_freq);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_SPEECH_VOLUME))) {
    s_speech_volume = clamp_pct(t->value->int32);
    persist_write_int(PERSIST_SPEECH_VOLUME, s_speech_volume);
  }
}
