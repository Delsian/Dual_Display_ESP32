#ifndef SPEECH_TEST_H
#define SPEECH_TEST_H
#include <stddef.h>
#include <stdint.h>

void request_speech_test();
// Voice activation must pause while offline, unconfigured, or a request is pending.
bool audio_reply_available();
// Copies the recording before returning; the audio task retains its buffer.
bool request_audio_reply(const uint8_t *stereo, size_t bytes);
// Called only by the audio task when idle; caller owns and frees returned PCM.
uint8_t *take_speech_test(size_t &bytes);
#endif
