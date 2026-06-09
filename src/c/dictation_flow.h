#pragma once
#include <pebble.h>

// Hard cap on usable transcript length. Mainly bounds the watch->phone
// AppMessage payload now -- the TTS audio is streamed through a small fixed ring
// (see audio_playback.h), so phrase length no longer has to fit a per-phrase
// buffer. Raised 80 -> 200 once streaming removed that ceiling, so longer (and
// fast-spoken) sentences aren't truncated mid-dictation. Stays well under the
// 1024-byte outbox.
#define TRANSCRIPT_MAX_CHARS 200

typedef enum {
  DICTATION_RESULT_SUCCESS,    // `transcript` holds the capped, NUL-terminated text
  DICTATION_RESULT_CANCELLED,  // user backed out / nothing heard -- return to idle quietly
  DICTATION_RESULT_ERROR,      // connectivity/system/internal failure -- show a message
} DictationResult;

// `transcript` is only valid (non-NULL) when result == DICTATION_RESULT_SUCCESS,
// and only for the duration of the callback -- copy it if you need to keep it
// (it points at dictation_flow's own static buffer, reused on the next session).
typedef void (*DictationResultCallback)(DictationResult result, const char *transcript);

// Creates the underlying DictationSession. Safe to call even where dictation
// isn't supported (session will be NULL; dictation_flow_start then reports
// DICTATION_RESULT_ERROR immediately rather than crashing).
void dictation_flow_init(void);
void dictation_flow_deinit(void);

// True if a session could be created (mic + paired phone w/ dictation support).
bool dictation_flow_is_available(void);

// Shows the system dictation UI. `callback` fires once with the outcome.
// No-op (silently ignored) if a session is already in progress.
void dictation_flow_start(DictationResultCallback callback);
