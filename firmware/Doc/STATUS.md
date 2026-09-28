# Current project status

Updated: 2026-09-28. Scope: 16 kHz clips regenerated and firmware/filesystem uploaded.

## Implemented and build-tested

- Added validated manual BLE playback, queued to the voice task with busy and
  connection-generation guards. Existing recording/reply flow is unchanged.
- Target build and clip host tests passed; Android build and 14 unit tests passed.
- Wi-Fi removed; serial `speech` commands run in a dedicated task.
- Config retains version/volume; old fields are ignored at boot and omitted
  on save. BLE patches reject removed fields; voice protocol is unchanged.

- Recording jobs now pass through the requests queue to the BLE voice task.
- Voice service initializes on the same GATT server as config and battery,
  before advertising; connect/disconnect callbacks are wired in.
- Disconnect invalidates the recording's connection generation and subscription;
  reply waiting exits within its 100 ms polling interval. Failed streaming holds
  busy until capture finalizes, protecting the shared recording buffer.
- PlatformIO firmware build passed; existing speech clip/ADPCM tests passed
  with AddressSanitizer and UndefinedBehaviorSanitizer. BLE hardware unverified.
- 16 kHz mono IMA ADPCM: all 60 clips regenerated; sanitized decoder tests
  passed and every clip matched ffmpeg. Filesystem and firmware builds passed.
- Firmware + filesystem uploaded to /dev/ttyACM0 with verified flash hashes.

## Hardware evidence and current limits
- Native Android relay source exists in the sibling project; its build/device
  evidence is tracked in [Android status](../../android/Doc/STATUS.md).
- Voice activation previously confirmed by the user. Defaults: 80 ms onset,
  up to 200 ms pre-roll, 800 ms silence cutoff, five-second maximum,
  one-second cooldown. KEY1 remains available.
- `AUDIO_AI_REPLY_TEST=1`, `AUDIO_VOICE_ACTIVATION=1`, microphone channel 0.
- Energy detection can trigger on background sounds; tune `AUDIO_VAD_MIN_RMS`.
- Worker retired; Android calls Gemini directly. BLE contract is unchanged.
- Topic generator syntax/catalog parity passed; live Gemini/hardware checks pending.

## Pending work
- Current voice protocol documented in [BLE.md](BLE.md) on 2026-09-25;
  finalization and integration validation remain pending. See [handoff](../proposed_changes.md).
- Firmware flashed; install updated app and check manual play, missing clips, busy commands
  and disconnect/reconnect. No playback result notification is implemented.
- Phone/ESP32 validation: pairing, subscription, streaming, disconnect during
  capture/reply wait, reconnect/resubscribe, clip replies and actual latency.
- All 60 clips fit: data/ uses 3.18 of 3.38 MB; partition layout unchanged.
  Filesystem upload applied local audio_volume=75; device reset completed.
- Verify audible playback speed, interruption and recording after playback.
- Validate real-audio topic/ignore selection through Android; no Worker deployment needed.
- GitButler session branch unavailable: checkout reports setup required.
  Changes remain uncommitted; no branch setup or history changes performed.
