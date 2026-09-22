# Parrot project context

## Goal and priorities

Build a voice companion on the Waveshare ESP32-S3-DualEye-Touch-LCD-1.28:
record a spoken phrase, obtain an AI answer, and play the spoken response while
keeping the animated eyes responsive. Use a cloud backend without a dedicated
PC/Raspberry Pi. Prefer free operation where available and the lowest practical
model cost; answer quality is secondary. Reduce conversational pauses.

Implement and validate one step at a time. Build/host tests precede hardware
validation by the user. See [STATUS.md](STATUS.md) for the current stage.

## Architecture and source map

- Arduino/PlatformIO firmware; environment `esp32-s3-dualeye-touch-lcd-1_28`.
- [Audio task](../src/audio.cpp): ES7210 microphone and ES8311 speaker over I2S,
  16 kHz stereo capture, PSRAM recording buffer, KEY1 and automatic activation.
- [Sound detector](../src/audio_vad.cpp): adaptive energy threshold on the
  selected microphone channel; not a wake-word or semantic speech detector.
- [Network task](../src/speech_test.cpp): authenticated HTTPS to the Worker,
  one request at a time, persistent connection reuse, serial replies/timings.
- [WAV helpers](../src/speech_wav.cpp): mono 16 kHz upload and conversion of
  generated 24 kHz mono audio to 16 kHz stereo playback.
- [Cloudflare Worker](../backend/worker.mjs): Groq transcription/replies and Gemini
  TTS behind a device token;
  `/health`, `/test-ai`, `/test-speech`, `/ask`, and `/speak`. Deployment is manual
  through Cloudflare. Service: `https://parrot.eug-krashtan.workers.dev`.
- [Firmware settings](../include/config.h), [runtime configuration](CONFIG.md),
  [Wi-Fi setup](WIFI.md), and [audio operation/testing](SPEECH.md).

## Decisions and constraints

- AI replies must use English or Ukrainian only; `/ask` sends this as a Groq
  system instruction. Unsupported languages receive an English language reminder.
  Unrecognized/absent speech is ignored: the Worker maps `[IGNORE]` to an empty
  answer, which firmware skips for TTS.
- Firmware prints the `/ask` answer, then posts its text to `/speak` for TTS and
  playback. Speech is bounded to ten seconds; failures preserve the serial answer.
- Keep Wi-Fi modem sleep enabled when BLE is active: disabling it caused a
  Wi-Fi driver abort on hardware. Retain HTTPS certificate validation.
- Reuse HTTPS connections to avoid repeated handshake delay; do not automatically
  retry POSTs that could already have been processed by the provider.
  Idle connections receive health requests every 25 seconds, stopping three
  minutes after the last user network job completes; pings do not extend this limit.
- Capture ends at five seconds or silence; upload currently starts afterward.
  Requests are independent, without conversation history.
- Provider credentials stay in Worker secrets. Local `.dev.vars` and
  `include/speech_secrets.h` contain secrets and must not be copied into context,
  logs, or commits. Firmware uses the device token, not provider API keys.
- Model identifiers live in the Worker. Their presence in source does not prove
  current availability, free quota, or lowest pricing; verify before changing.
- Firmware builds, filesystem uploads, and Worker deployments are separate.
  Follow the relevant test instructions; do not assume local edits are deployed.

## Validation

Build: `pio run -e esp32-s3-dualeye-touch-lcd-1_28`.
Host tests: `test/audio_vad_test.cpp` and `test/speech_wav_test.cpp` with their
corresponding source files and `-Iinclude`; see [SPEECH.md](SPEECH.md).
Hardware checks must establish microphone sensitivity, playback, Wi-Fi/BLE
coexistence, animation responsiveness, and actual latency.
