#include <pebble.h>
#include "config.h"
#include "parrot_window.h"
#include "dictation_flow.h"
#include "audio_playback.h"
#include "phrase_menu_window.h"
#include "polly.h"

// True from the moment we send a TRANSCRIPT until the AUDIO_* round trip ends
// (success, TTS_ERROR, or a transport failure). Lets the inbox dispatcher tell
// a legitimate AUDIO_* message apart from a stray late one after an abort, and
// lets transport-failure handlers know whether they interrupted a TTS request.
static bool s_audio_expected;

// --- Outbound: transcript -> phone -------------------------------------------

static void send_transcript(const char *text) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    return;
  }
  dict_write_cstring(out, MESSAGE_KEY_TRANSCRIPT, text);
  app_message_outbox_send();
}

// Sends a question for the phone to route to the AI (instead of speaking it
// verbatim). The phone replies with ANSWER (shown in the bubble) + the AUDIO_*
// stream of the spoken answer.
static void send_question(const char *text) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    return;
  }
  dict_write_cstring(out, MESSAGE_KEY_QUESTION, text);
  app_message_outbox_send();
}

// Flow control for streamed audio: tell the phone the highest chunk index we
// can currently accept. audio_playback raises this as it drains its ring, so
// delivery self-throttles to the playback rate. Returns whether the request
// went out (false on a busy outbox -- audio_playback retries on the next pump).
static bool send_chunk_request(int up_to_index) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    return false;
  }
  dict_write_int32(out, MESSAGE_KEY_CHUNK_REQUEST, up_to_index);
  return app_message_outbox_send() == APP_MSG_OK;
}

// --- Speak an arbitrary string (shared by dictation + preset phrases) ---------

static void speak_text(const char *text) {
  parrot_window_show_transcript(text);
  parrot_window_set_state(UI_STATE_THINKING);
  s_audio_expected = true;
  send_transcript(text);
}

void polly_speak_phrase(const char *text) {
  if (!text || !text[0] || parrot_window_get_state() != UI_STATE_IDLE) {
    return;
  }
  speak_text(text);
}

// --- Dictation result ---------------------------------------------------------

static void on_dictation_result(DictationResult result, const char *transcript) {
  switch (result) {
    case DICTATION_RESULT_SUCCESS:
      speak_text(transcript);
      break;

    case DICTATION_RESULT_CANCELLED:
      parrot_window_set_state(UI_STATE_IDLE);
      break;

    case DICTATION_RESULT_ERROR:
      parrot_window_show_message("Couldn't hear you");
      parrot_window_set_state(UI_STATE_ERROR);
      break;
  }
}

// AI variant: the dictated text is a question -> ask the phone's AI, then speak
// the answer (rather than speaking the question verbatim).
static void on_ai_dictation_result(DictationResult result, const char *transcript) {
  switch (result) {
    case DICTATION_RESULT_SUCCESS:
      parrot_window_show_transcript(transcript);   // show the question while thinking
      parrot_window_set_state(UI_STATE_THINKING);
      s_audio_expected = true;
      send_question(transcript);
      break;

    case DICTATION_RESULT_CANCELLED:
      parrot_window_set_state(UI_STATE_IDLE);
      break;

    case DICTATION_RESULT_ERROR:
      parrot_window_show_message("Couldn't hear you");
      parrot_window_set_state(UI_STATE_ERROR);
      break;
  }
}

// Polly only talks. With the watch's speaker muted (Settings → Sounds &
// Haptics, or Quiet Time) the firmware would play every phrase into silence
// after a full dictation and TTS round trip, so say so up front instead.
static bool speaker_blocked(void) {
  if (!speaker_is_muted()) {
    return false;
  }
  parrot_window_show_message("Watch speaker is muted");
  parrot_window_set_state(UI_STATE_ERROR);
  return true;
}

static void on_select_pressed(void) {
  if (parrot_window_get_state() != UI_STATE_IDLE) {
    return;
  }
  if (speaker_blocked()) {
    return;
  }
  if (!dictation_flow_is_available()) {
    parrot_window_show_message("No mic available");
    parrot_window_set_state(UI_STATE_ERROR);
    return;
  }
  parrot_window_set_state(UI_STATE_LISTENING);
  dictation_flow_start(on_dictation_result);
}

static void on_up_pressed(void) {
  if (parrot_window_get_state() != UI_STATE_IDLE) {
    return;
  }
  if (speaker_blocked()) {
    return;
  }
  if (!dictation_flow_is_available()) {
    parrot_window_show_message("No mic available");
    parrot_window_set_state(UI_STATE_ERROR);
    return;
  }
  parrot_window_set_state(UI_STATE_LISTENING);
  dictation_flow_start(on_ai_dictation_result);
}

static void on_down_pressed(void) {
  if (parrot_window_get_state() != UI_STATE_IDLE) {
    return;
  }
  if (speaker_blocked()) {
    return;
  }
  if (config_preset_count() == 0) {
    parrot_window_show_message("No phrases set");
    parrot_window_set_state(UI_STATE_ERROR);
    return;
  }
  phrase_menu_window_push(polly_speak_phrase);
}

// BACK: from any non-idle state (error, thinking, speaking) return to the idle
// start screen instead of quitting; abort any in-flight audio. From idle, BACK
// exits the app as usual.
static void on_back_pressed(void) {
  if (parrot_window_get_state() == UI_STATE_IDLE) {
    window_stack_pop(true);
    return;
  }
  s_audio_expected = false;
  audio_playback_abort();
  parrot_window_set_state(UI_STATE_IDLE);
}

// --- Playback completion -------------------------------------------------------

static void on_playback_done(bool success) {
  s_audio_expected = false;
  if (!success) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "polly: playback finished unsuccessfully");
  }
  parrot_window_set_state(UI_STATE_IDLE);
}

// --- Inbound: AUDIO_* round trip ----------------------------------------------

static void handle_tts_error(Tuple *t_error) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "polly: TTS_ERROR=%ld", (long)config_tuple_int(t_error));
  s_audio_expected = false;
  audio_playback_abort();
  parrot_window_show_message("Couldn't speak that");
  parrot_window_set_state(UI_STATE_ERROR);
}

static void handle_audio_format(DictionaryIterator *iter) {
  Tuple *t_format = dict_find(iter, MESSAGE_KEY_AUDIO_FORMAT);
  Tuple *t_total = dict_find(iter, MESSAGE_KEY_AUDIO_TOTAL_LEN);
  if (!s_audio_expected || !t_format || !t_total) {
    return;
  }
  // Streamed playback: begin allocates only a small ring and grants the phone
  // its first window of chunks; the speaker starts once enough has buffered.
  if (audio_playback_begin((uint32_t)config_tuple_int(t_total), (int)config_tuple_int(t_format),
                           send_chunk_request, on_playback_done)) {
    parrot_window_set_state(UI_STATE_SPEAKING);
  } else {
    s_audio_expected = false;
    parrot_window_show_message("Audio too large");
    parrot_window_set_state(UI_STATE_ERROR);
  }
}

static void handle_audio_chunk(DictionaryIterator *iter) {
  Tuple *t_index = dict_find(iter, MESSAGE_KEY_AUDIO_CHUNK_INDEX);
  Tuple *t_chunk = dict_find(iter, MESSAGE_KEY_AUDIO_CHUNK);
  if (!s_audio_expected || !t_index || !t_chunk) {
    return;
  }
  audio_playback_append_chunk((uint32_t)config_tuple_int(t_index), t_chunk->value->data,
                              t_chunk->length);
}

// --- AppMessage plumbing -------------------------------------------------------

// One inbox handler serves both the TTS round trip (TRANSCRIPT's reply) and
// Clay settings updates -- they arrive on the same channel, so dispatch on
// whichever keys are present rather than wiring up two separate registrations.
static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;

  if ((t = dict_find(iter, MESSAGE_KEY_TTS_ERROR))) {
    handle_tts_error(t);
    return;
  }
  // The AI's answer text, to show in the bubble while Polly speaks it. Only
  // honoured mid-round-trip so a stray late message can't hijack the bubble.
  if ((t = dict_find(iter, MESSAGE_KEY_ANSWER))) {
    if (s_audio_expected) {
      parrot_window_show_transcript(t->value->cstring);
    }
    return;
  }
  if (dict_find(iter, MESSAGE_KEY_AUDIO_FORMAT)) {
    handle_audio_format(iter);
    return;
  }
  if (dict_find(iter, MESSAGE_KEY_AUDIO_CHUNK)) {
    handle_audio_chunk(iter);
    return;
  }

  config_inbox_received(iter, context);
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "polly: inbox dropped (%d)", (int)reason);
  if (s_audio_expected) {
    s_audio_expected = false;
    audio_playback_abort();
    parrot_window_show_message("Lost the connection");
    parrot_window_set_state(UI_STATE_ERROR);
  }
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "polly: outbox failed (%d)", (int)reason);
  if (s_audio_expected) {
    s_audio_expected = false;
    parrot_window_show_message("Couldn't reach your phone");
    parrot_window_set_state(UI_STATE_ERROR);
  }
}

// --- App lifecycle --------------------------------------------------------------

// One app-wide subscription powers the touch sensor; the events go to whichever
// window is on top. (The system navigation bridge crashes the app on firmware
// 4.33.1 — see the note in init().)
static void touch_dispatch(const TouchEvent *event, void *context) {
  if (phrase_menu_window_is_top())  { phrase_menu_window_handle_touch(event); }
  else if (parrot_window_is_top())  { parrot_window_handle_touch(event); }
}

static void init(void) {
  // No app_touch_navigation_enable() here: on firmware 4.33.1 the system touch
  // bridge faults inside firmware on the first touch and kills the app. Buttons
  // only until a later firmware fixes it.
  srand(time(NULL));   // the chirp picks a pattern at random (parrot_anim.c)
  config_load();
  dictation_flow_init();
  parrot_window_set_select_handler(on_select_pressed);
  parrot_window_set_up_handler(on_up_pressed);
  parrot_window_set_down_handler(on_down_pressed);
  parrot_window_set_back_handler(on_back_pressed);
  parrot_window_push();
  touch_service_subscribe(touch_dispatch, NULL);

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);
  // Larger inbox than a typical settings-only app: AUDIO_CHUNK carries binary
  // PCM payloads alongside the small Clay/transcript tuples.
  app_message_open(2048, 1024);
}

static void deinit(void) {
  audio_playback_abort();
  dictation_flow_deinit();
  parrot_window_destroy();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
