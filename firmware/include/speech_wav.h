#ifndef SPEECH_WAV_H
#define SPEECH_WAV_H

#include <stddef.h>
#include <stdint.h>

// Accept the Worker's canonical 24 kHz, mono, signed 16-bit PCM WAV only.
size_t speech_wav_frames(const uint8_t *wav, size_t bytes);
bool speech_wav_convert(const uint8_t *wav, size_t bytes, int16_t *stereo,
                        size_t output_frames);
// Extract one microphone channel from 16 kHz stereo into a canonical mono WAV.
bool recording_wav(const uint8_t *stereo, size_t bytes, unsigned channel,
                   uint8_t *wav, size_t wav_bytes);

#endif
