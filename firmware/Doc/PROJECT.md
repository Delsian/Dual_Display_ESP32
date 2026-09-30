# Parrot project context

## Goal and priorities

Build a voice companion on the Waveshare ESP32-S3-DualEye-Touch-LCD-1.28:
record a spoken phrase, obtain an AI answer, and play the spoken response while
keeping the animated eyes responsive. Call Gemini from the Android app without a dedicated
PC/Raspberry Pi. Prefer free operation where available and the lowest practical
model cost; answer quality is secondary. Reduce conversational pauses.

Implement and validate one step at a time. Build/host tests precede hardware
validation by the user. See [STATUS.md](STATUS.md) for the current stage.

## Architecture and source map

- Arduino/PlatformIO firmware; environment `esp32-s3-dualeye-touch-lcd-1_28`.
- [Audio task](../src/audio.cpp): ES7210 microphone and ES8311 speaker over I2S,
  16 kHz stereo capture, PSRAM recording buffer, KEY1 and automatic activation.
- [Device logs](../src/device_log.cpp): application output mirrors to serial and an
  authenticated, bounded BLE notification stream; protocol and client handoff in [BLE.md](BLE.md).
- [Sound detector](../src/audio_vad.cpp): adaptive energy threshold on the
  selected microphone channel; not a wake-word or semantic speech detector.
- [Voice task](../src/speech_test.cpp): BLE recordings to a native Android relay
  in the sibling Android project, one request at a time; phone returns a clip choice.
- [Android Gemini classifier](../../android/app/src/main/java/com/eugkrashtan/parrot/GeminiIntent.kt):
  the phone calls Gemini directly and selects a clip. No Cloudflare dependency or TTS.
- [Topic clips](../backend/topics.json): up to ~100 fixed topics; answers are
  produced externally and converted by [convert_phrases.py](../backend/convert_phrases.py)
  into [IMA ADPCM clips](../src/speech_clip.cpp) in `data/clips/`. The script also
  regenerates the Android topic asset (descriptions only); rebuild the app afterward.
- [Firmware settings](../include/config.h), [runtime configuration](CONFIG.md),
  [audio operation/testing](SPEECH.md), and
  [online services, billing and logs](SERVICES.html).

## Decisions and constraints

- Target: voice reply 1–2 s after speech ends. Android submits recordings
  directly to Gemini; Gemini may only output a topic number, `offtopic` or `ignore`
  (enum schema). The device plays `clips/NNN.wav`, a random `off_K.wav`, or nothing.
  No generation or TTS. Clips are 16 kHz mono IMA ADPCM (ffmpeg's
  exact `(2n+1)·step/8` form) in the 3.4 MB LittleFS partition; OTA slots kept.
  Playback and microphone capture both use 16 kHz.
- Unrecognized/absent speech maps to `ignore`. Replies are prerecorded, not generated.
  Firmware has no TTS/WAV-download path;
  serial `speech [N|off_K]` plays a local clip (random if omitted), no network needed.
  Android also requests manual stored-clip playback over BLE; see [BLE.md](BLE.md).
- BLE-only voice transport, with no Wi-Fi fallback. Native Android 12+ relay
  source is in `../android`; Android build and device validation are tracked there.
  Wi-Fi provisioning, settings and dependencies were removed on 2026-09-27.
  Local serial clip playback runs in a separate task. Boot starts a five-minute
  active window; BLE disconnect clears manual sleep and starts the same window.
  Each played response restarts it. While disconnected and active,
  captured speech gets a random local fallback. A connected phone keeps the device
  active. Once the offline window expires, eyes/audio turn off; after one hour in
  disconnected idle, deep sleep disables BLE until the reset button restarts it.
  App Sleep forces eyes/audio off while retaining BLE; WakeUp restores activity.
- Capture ends at five seconds or silence. BLE streams packetized IMA ADPCM
  while recording; the Android relay decodes to PCM and calls Gemini.
  Requests are independent, without conversation history. Protocol finalization
  and phone validation remain pending; see [handoff](../proposed_changes.md).
- The Android app manages its Gemini API key in Settings and private storage.
  Firmware stores no provider key, device token, or root CA. Local `.dev.vars`
  is legacy secret material; never copy it into context, logs, or commits.
- Model identifiers live in the Android classifier. Their presence in source does not prove
  current availability, free quota, or lowest pricing; verify before changing.
- Firmware builds, filesystem uploads, and Android builds are separate.
  Follow the relevant test instructions; do not assume local edits are deployed.

## Integration boundaries

- BLE firmware-only OTA v1 is specified in [BLE.md](BLE.md#ble-ota-v1-contract-transfer-implemented).
  Begin/Data/Finish with hash/image verification and reboot are implemented;
  Linux test uploader is in [scripts](../scripts/README.md); Android uploader evidence
  is in [Android status](../../android/Doc/STATUS.md). Hardware rollback checks remain pending.
  Pending images are confirmed only after
  startup health checks; failures/deadline request rollback. Preserve NVS/LittleFS.
  Versions in `include/firmware_version.h` use numeric major.minor.patch; OTA Finish
  only activates a higher Parrot version. Recovery rollback is exempt. The running
  version is readable through the standard BLE Firmware Revision characteristic.
- BLE pairing uses Secure Connections with MITM protection and persistent NVS
  bonding for at least two phones, with one active connection at a time.
  Initial pairing requires the displayed passkey; reconnects resubscribe
  to notifications while reusing stored keys. See BLE.md for recovery/validation.

[BLE.md](BLE.md) is the authoritative shared BLE contract. Firmware owns capture
and clip playback; Android owns its UI, permissions, BLE client, decoding, and
HTTP client. Read Android context only for specific integration questions;
do not duplicate its implementation status here. For cross-project changes,
hand off contract impact, required counterpart changes, and validation evidence.

## Validation

Build: `pio run -e esp32-s3-dualeye-touch-lcd-1_28`.
Host tests: `test/audio_vad_test.cpp` and `test/speech_clip_test.cpp` with their
corresponding source files and `-Iinclude`; Android owns classifier tests;
see [SPEECH.md](SPEECH.md).
Hardware checks must establish microphone sensitivity, playback, BLE
connectivity, animation responsiveness, and actual latency.
