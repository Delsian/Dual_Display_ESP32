# Current project status

Updated: 2026-09-27. Scope: context boundaries; prior BLE validation retained.

## Implemented and build-tested

- Recording jobs now pass through the requests queue to the BLE voice task.
- Voice service initializes on the same GATT server as config and battery,
  before advertising; connect/disconnect callbacks are wired in.
- Disconnect invalidates the recording's connection generation and subscription;
  reply waiting exits within its 100 ms polling interval. Failed streaming holds
  busy until capture finalizes, protecting the shared recording buffer.
- PlatformIO firmware build passed; existing speech clip/ADPCM tests passed
  with AddressSanitizer and UndefinedBehaviorSanitizer. BLE hardware unverified.
- 24 kHz mono IMA ADPCM clips: previous firmware/filesystem builds and sanitized
  decoder tests passed; all 24 regenerated clips matched ffmpeg exactly.
  Serial `speech [N|off_K]` plays local clips; I2S restores 16 kHz afterward.

## Hardware evidence and current limits

- Native Android relay source exists in the sibling project; its build/device
  evidence is tracked in [Android status](../../android/Doc/STATUS.md).
- Context instructions now limit cross-project reading. This documentation
  update does not add firmware build or hardware verification evidence.
- Earlier Wi-Fi `/ask`, TTS playback and HTTPS reuse worked in user logs;
  these results do not validate the replacement BLE transport. TTS was removed.
- Voice activation previously confirmed by the user. Defaults: 80 ms onset,
  up to 200 ms pre-roll, 800 ms silence cutoff, five-second maximum,
  one-second cooldown. KEY1 remains available.
- `AUDIO_AI_REPLY_TEST=1`, `AUDIO_VOICE_ACTIVATION=1`, microphone channel 0.
- Energy detection can trigger on background sounds; tune `AUDIO_VAD_MIN_RMS`.
- Worker uses Gemini 3.6 Flash; last reported Worker validation: 5 tests passed.
- Wi-Fi setup still exists; keep modem sleep enabled until removal, because
  disabling it previously caused a driver abort with BLE active.

## Pending work

- Current voice protocol documented in [BLE.md](BLE.md) on 2026-09-25;
  finalization and integration validation remain pending. See [handoff](../proposed_changes.md).
- Remove remaining Wi-Fi setup separately. Worker stays unchanged for the relay.
- Phone/ESP32 validation: pairing, subscription, streaming, disconnect during
  capture/reply wait, reconnect/resubscribe, clip replies and actual latency.
- Upload filesystem + firmware; check pitch/speed, KEY1 interruption,
  recording after playback, background noise and continued animation.
- 24 of 60 clips available; data/ uses 2.02 of 3.38 MB. Add missing phrases.
- Worker redeployment and real-audio language/ignore validation remain pending.
- GitButler session branch unavailable: checkout reports setup required.
  Changes remain uncommitted; no branch setup or history changes performed.
