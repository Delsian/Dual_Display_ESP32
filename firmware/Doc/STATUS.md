# Current project status

Updated: 2026-09-29. Scope: audio stack-overflow fix built; hardware retest pending.

## Implemented and build-tested

- Boot, BLE disconnect, and each spoken reply start a five-minute active window.
  App Sleep turns eyes/audio off; WakeUp or disconnect clears the sleep override.
  Offline capture plays random off_*.wav fallback clips; connected relay flow stays.
  Window expiry turns off eyes/audio; one subsequent idle hour sleeps until RESET.
  Target build and active-window host checks passed; hardware validation pending.
- Secure Connections/MITM bonding enabled with encryption/identity key distribution.
  Target build passed; reconnect/reboot bond reuse still needs hardware validation.
  Android needs no code change; forget any obsolete phone pairing and pair once again.
- Authenticated BLE log notifications mirror application serial output through a
  bounded queue/background task, with sequence/drop counters. Passkeys stay serial-only.
  Target build and sanitized buffer tests passed (fragmentation, overflow, reset/wrap).
  See [BLE contract and client handoff](BLE.md#device-log-service); Android log UI is pending.
- Both eyes blink together after random 3–7 s open intervals: 80 ms closing,
  40 ms closed, 140 ms opening. Timing is nonblocking and rollover-safe.
  Target firmware build passed; blink upload and hardware validation pending.
- Added validated manual BLE playback, queued to the voice task with busy and
  connection-generation guards. Existing recording/reply flow is unchanged.
- Target build and clip host tests passed; Android build and 14 unit tests passed.
- Wi-Fi removed; serial `speech` commands run in a dedicated task.
- Config retains version/volume; old fields are ignored at boot and omitted
  on save. BLE patches reject removed fields; voice protocol is unchanged.

- Audio stack-canary crash matched ELF 6125ac863604268d. Buffers moved off stack;
  Stack is now 8 KB, frame 2208 → 160 bytes; build passed. Hardware retest pending.
- Disconnect invalidates the recording's connection generation and subscription;
  reply waiting exits within its 100 ms polling interval. Failed streaming holds
  busy until capture finalizes, protecting the shared recording buffer.
- PlatformIO firmware build passed; existing speech clip/ADPCM tests passed
  with AddressSanitizer and UndefinedBehaviorSanitizer. BLE hardware unverified.
- 16 kHz mono IMA ADPCM: all 60 clips regenerated; sanitized decoder tests
  passed and every clip matched ffmpeg. Filesystem and firmware builds passed.

## Hardware evidence and current limits
- Native Android relay source exists in the sibling project; its build/device
  evidence is tracked in [Android status](../../android/Doc/STATUS.md).
- Voice activation previously confirmed by the user. Defaults: 80 ms onset,
  up to 200 ms pre-roll, 800 ms silence cutoff, five-second maximum,
  one-second cooldown. KEY1 remains available.
- `AUDIO_AI_REPLY_TEST=1`, `AUDIO_VOICE_ACTIVATION=1`, microphone channel 0.
- Energy detection can trigger on background sounds; tune `AUDIO_VAD_MIN_RMS`.
- Log upload/hardware checks pending: pairing gate, MTUs, reconnects, loss handling,
  and concurrent audio/animation. Existing clients require no counterpart changes.
- Topic generator syntax/catalog parity passed; live Gemini/hardware checks pending.
