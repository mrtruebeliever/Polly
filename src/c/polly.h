#pragma once

// Speaks an arbitrary string verbatim through the phone-TTS -> speaker pipeline
// (same path as a successful dictation). Used by the preset-phrase menu.
void polly_speak_phrase(const char *text);
