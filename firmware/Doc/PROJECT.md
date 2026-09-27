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
- [Voice task](../src/speech_test.cpp): BLE recordings to a native Android relay
  in the sibling Android project, one request at a time; phone returns a clip choice.
- [Cloudflare Worker](../backend/worker.mjs): Gemini only, behind a device token;
  `/health`, `/test-ai`, `/ask`, `/intent`. No TTS. Deployment is manual
  through Cloudflare. Service: `https://parrot.eug-krashtan.workers.dev`.
- [Topic clips](../backend/topics.json): up to ~100 fixed topics; answers are
  produced externally and converted by [convert_phrases.py](../backend/convert_phrases.py)
  into [IMA ADPCM clips](../src/speech_clip.cpp) in `data/clips/`. The script also
  regenerates the Worker topic list (descriptions only).
- [Firmware settings](../include/config.h), [runtime configuration](CONFIG.md),
  [Wi-Fi setup](WIFI.md), [audio operation/testing](SPEECH.md), and
  [online services, billing and logs](SERVICES.html).

## Decisions and constraints

- Target: voice reply 1–2 s after speech ends. Android forwards recordings
  to `/intent`; Gemini may only output a topic number, `offtopic` or `ignore`
  (enum schema). The device plays `clips/NNN.wav`, a random `off_K.wav`, or nothing.
  No generation or TTS. Clips are 24 kHz mono IMA ADPCM (ffmpeg's
  exact `(2n+1)·step/8` form) in the 3.4 MB LittleFS partition; OTA slots kept.
  Playback switches I2S to 24 kHz and restores 16 kHz for microphone capture.
- AI replies must use English or Ukrainian only; `/ask` sends this as a Gemini
  system instruction. Unsupported languages receive an English language reminder.
  Unrecognized/absent speech is ignored: the Worker maps `[IGNORE]` to an empty answer.
- 2026-09-25: Worker TTS (`/speak`, `/reply`, `/test-speech`) removed at user request;
  `GOOGLE_TTS_API_KEY` is no longer used. Firmware has no TTS/WAV-download path;
  serial `speech [N|off_K]` plays a local clip (random if omitted), no network needed.
- Keep Wi-Fi modem sleep enabled when BLE is active: disabling it caused a
  Wi-Fi driver abort on hardware. Retain HTTPS certificate validation.
- BLE-only voice transport, with no Wi-Fi fallback. Native Android 12+ relay
  source is in `../android`; Android build and device validation are tracked there.
  Existing Wi-Fi setup remains until a separate removal step.
- Capture ends at five seconds or silence. BLE streams packetized IMA ADPCM
  while recording; the Android relay decodes to PCM and calls the unchanged Worker.
  Requests are independent, without conversation history. Protocol finalization
  and phone validation remain pending; see [handoff](../proposed_changes.md).
- Provider credentials stay in Worker secrets. The Android relay accepts the
  device token. Local `.dev.vars` and `include/speech_secrets.h` contain secrets
  and must not be copied into context, logs, or commits.
- Model identifiers live in the Worker. Their presence in source does not prove
  current availability, free quota, or lowest pricing; verify before changing.
- Firmware builds, filesystem uploads, and Worker deployments are separate.
  Follow the relevant test instructions; do not assume local edits are deployed.

## Integration boundaries

[BLE.md](BLE.md) is the authoritative shared BLE contract. Firmware owns capture
and clip playback; Android owns its UI, permissions, BLE client, decoding, and
HTTP client. Read Android context only for specific integration questions;
do not duplicate its implementation status here. For cross-project changes,
hand off contract impact, required counterpart changes, and validation evidence.

## Validation

Build: `pio run -e esp32-s3-dualeye-touch-lcd-1_28`.
Host tests: `test/audio_vad_test.cpp` and `test/speech_clip_test.cpp` with their
corresponding source files and `-Iinclude`; Worker: `node --test test/worker*.mjs`;
see [SPEECH.md](SPEECH.md).
Hardware checks must establish microphone sensitivity, playback, Wi-Fi/BLE
coexistence, animation responsiveness, and actual latency.
