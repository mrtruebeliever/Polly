#include <pebble.h>
#include "config.h"
#include "parrot_window.h"
#include "dictation_flow.h"
#include "audio_playback.h"

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

// --- Dictation result ---------------------------------------------------------

static void on_dictation_result(DictationResult result, const char *transcript) {
  switch (result) {
    case DICTATION_RESULT_SUCCESS:
      parrot_window_show_transcript(transcript);
      parrot_window_set_state(UI_STATE_THINKING);
      s_audio_expected = true;
      send_transcript(transcript);
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

static void on_select_pressed(void) {
  if (parrot_window_get_state() != UI_STATE_IDLE) {
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
  APP_LOG(APP_LOG_LEVEL_WARNING, "polly: TTS_ERROR=%ld", (long)t_error->value->int32);
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
  if (!audio_playback_begin((uint32_t)t_total->value->int32, (int)t_format->value->int32)) {
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
  audio_playback_append_chunk((uint32_t)t_index->value->int32, t_chunk->value->data,
                              t_chunk->length);
}

static void handle_audio_done(void) {
  if (!s_audio_expected) {
    return;
  }
  // Hand the PCM to the speaker (which copies it) and free the up-to-64KB
  // reassembly buffer FIRST, then load the SPEAK pose bitmaps. Doing it the
  // other way around keeps both resident at once and overflows emery's heap
  // for longer phrases -- the corrupt-bitmap crash this whole flow caused.
  // On a synchronous failure audio_playback_finish already drove the UI via
  // on_playback_done, so don't force SPEAKING on top of it.
  if (audio_playback_finish(on_playback_done)) {
    parrot_window_set_state(UI_STATE_SPEAKING);
  }
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
  if (dict_find(iter, MESSAGE_KEY_AUDIO_FORMAT)) {
    handle_audio_format(iter);
    return;
  }
  if (dict_find(iter, MESSAGE_KEY_AUDIO_CHUNK)) {
    handle_audio_chunk(iter);
    return;
  }
  if (dict_find(iter, MESSAGE_KEY_AUDIO_DONE)) {
    handle_audio_done();
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

static void init(void) {
  config_load();
  dictation_flow_init();
  parrot_window_set_select_handler(on_select_pressed);
  parrot_window_push();

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
