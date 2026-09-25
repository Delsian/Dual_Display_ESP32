#ifndef SPEECH_TEST_H
#define SPEECH_TEST_H
#include <stddef.h>
#include <stdint.h>

// Plays a LittleFS clip locally: "12" -> clips/012.wav, "off_3", or random if empty.
void request_speech_test(const char *clip);
// Voice activation must pause while offline, unconfigured, or a request is pending.
bool audio_reply_available();
// Audio task only. Starts uploading a recording while it is captured into
// `stereo`, which must stay unchanged until audio_reply_available() is true.
bool begin_audio_reply(const uint8_t *stereo);
// Publishes captured bytes; final ends the upload, cancelling it under 0.25 s.
void audio_reply_progress(size_t bytes, bool final);
// Decoded clip audio handed from the network or serial task to the audio task.
struct SpeechAudio;
// Called only by the audio task when idle; release the result when finished.
SpeechAudio *take_speech_audio();
// Returns PCM and the bytes available now; done means no more will arrive.
const uint8_t *speech_audio_data(SpeechAudio *audio, size_t &bytes, bool &done);
// Called once by the audio task; also stops an unfinished download.
void release_speech_audio(SpeechAudio *audio);
#endif
