> Superseded integration note (2026-09-28): Android now calls Gemini directly.
> Worker forwarding below is historical; see `Doc/BLE.md` and Android context.

# BLE voice link: handoff notes for the next agent

Goal: replace Wi-Fi/HTTPS as the transport for AI voice requests with BLE to a
native Android app. The app forwards recordings to the Cloudflare Worker
(`/intent`) and relays the Worker's clip choice back to the ESP32. Wi-Fi is
being removed entirely for this path (user decision — BLE only, no fallback).
The Worker itself does not change; only the firmware transport and a new
Android app are affected.

## Status of firmware edits (uncommitted, in this checkout)

Firmware integration completed 2026-09-25; phone/hardware validation pending:
- `include/speech_clip.h`, `src/speech_clip.cpp`: added a shared IMA ADPCM
  step function (`apply_nibble`) used by both the existing clip decoder and
  new `speech_adpcm_encode`/`speech_adpcm_decode` functions, for streaming one
  channel of mic audio over BLE in small packets. Each packet carries its own
  `{predictor, index}` header so a lost BLE packet doesn't corrupt later ones.
  Round-trip tested in `test/speech_clip_test.cpp` (tone + clicks, verified
  >20 dB SNR, packet size 319 samples to exercise odd nibble counts). This
  part is solid — build and run:
  `g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude src/speech_clip.cpp test/speech_clip_test.cpp -o /tmp/ct && /tmp/ct`
- `src/speech_test.cpp`: queue handoff and BLE callbacks completed. Replaced the
  Wi-Fi/HTTPS network task (`HttpSession`, `PersistentHttp`, chunked upload,
  keep-alive) with a BLE GATT service (`init_voice_link`, `voice_link_connected`,
  `voice_link_disconnected`, `reply_to_recording`, `voice_task`). Protocol
  sketch (not finalized, not validated against phone-side constraints):
  ```
  TX (notify, ESP32 -> phone): 0x01 start [rate u16]
                                0x02 audio [seq u8][predictor i16][index u8][nibbles]
                                0x03 end [samples u32]
                                0x04 cancel
  RX (write, phone -> ESP32):   UTF-8 "clip:<name>" | "ignore" | "error:<text>"
  ```
  New UUIDs (documented in `Doc/BLE.md`):
  `6b520010` service, `6b520011` TX (notify), `6b520012` RX (write).
  Packet size 320 samples (20 ms) chosen to fit a 165-byte notification under
  MTU 185, matching the existing `BLEDevice::setMTU(185)` in `ble_config.cpp`.

## Known gaps / next steps for whoever continues this

1. **Implemented:** `begin_audio_reply` queues `UploadJob*` for `voice_task`.
   Queue failure releases busy; transport failure retains the recording buffer
   until capture finalizes. Jobs retain their connection generation.
2. **Implemented:** `init_voice_link(server)` runs on the shared config/battery
   GATT server before advertising; declarations are in `speech_test.h`.
3. **Implemented:** connect/disconnect callbacks update the voice link and clear
   its subscription. Disconnect ends reply waiting within the 100 ms polling
   interval, rather than the 30-second timeout. During capture, buffer ownership
   stays reserved until finalization. Hardware verification pending.
4. **Wi-Fi removed 2026-09-27.** Provisioning, display, connection settings,
   WiFiManager and unused firmware HTTPS headers are gone. Local `speech`
   commands run in a dedicated serial task. Configuration clients must send
   only version/volume fields; the voice protocol is unchanged. The Android
   relay supplies the device token. Hardware verification remains pending.
5. **`speech_clip.h` comment says "24 kHz"** (`SPEECH_CLIP_SAMPLE_RATE = 24000`)
   — this was already true before this session's edits and is unrelated to
   the BLE work, but flows into `MAX_CLIP_BYTES`/`MAX_SPEECH_FRAMES` sizing
   the user flagged as inconsistent with the 3.4 MB LittleFS partition. Not
   addressed here; needs its own decision (16 kHz clips vs. bigger partition).
6. **Documented 2026-09-25:** `Doc/BLE.md` now describes the implemented voice
   service, packet layout, pairing/MTU requirements, relay replies and lifecycle.
   Protocol finalization and phone/hardware validation remain pending.
7. **No Android code exists yet.** See install list below; nothing has been
   scaffolded in-repo (no `android/` directory).

## Design decisions already made with the user (don't re-litigate)

- BLE only — no Wi-Fi fallback in firmware after this lands.
- Native Android app, not a web/PWA Bluetooth page.
- minSdk 31 (Android 12+) — new Bluetooth permission model only, no legacy
  `BLUETOOTH`/`BLUETOOTH_ADMIN`/location-based scan permission handling needed.
- Source-only for the Android app: no local Android SDK/Gradle toolchain is
  installed in this environment, so the app is written but not compiled here;
  the user builds/runs it in Android Studio and reports errors back.
- App calls the existing Worker `/intent` endpoint directly over HTTPS
  (device token embedded in the app instead of firmware); Worker is unchanged.

## What the Android app needs to do (not yet built)

1. Scan for `Parrot-XXXXXX`, connect, pair (reuse the passkey flow already in
   `ble_config.cpp` — display-only passkey, MITM, no bonding persisted by
   firmware, so app should expect to re-pair each connection).
2. Discover the voice service, subscribe to TX notifications, write RX.
3. On `0x01 start`: begin buffering PCM.
4. On each `0x02 audio` packet: decode the packet's IMA ADPCM nibbles back to
   PCM (mirror `speech_adpcm_decode`, same table/step logic) or, simpler,
   just re-encode as WAV directly from the raw ADPCM+header since the Worker
   only needs a valid WAV — decide whether the app decodes to PCM (simplest,
   reuses standard WAV writing) or forwards ADPCM into its own WAV container
   (saves a decode step but Worker's `recordingPart` expects PCM/`audio/L16`,
   so PCM is likely required, at least until the Worker is changed).
5. On `0x03 end`: finalize the WAV, POST to `/intent`, wait for JSON.
6. Write `clip:<name>` / `ignore` / `error:<text>` back to RX depending on the
   Worker's response.
7. On `0x04 cancel` from the ESP32: abort the in-flight upload without
   forwarding anything.

## Install list for adding the Android project to this repo

Given to the user separately as a plain list (Sonnet, this session, no
classifier issue on the list itself — re-send if needed):

- **JDK 17** (Android Gradle Plugin 8.x requires it) — e.g.
  `sudo apt install openjdk-17-jdk` or Android Studio's bundled JBR.
- **Android Studio** (recommended, includes SDK manager, emulator, Gradle
  wrapper support) — download from developer.android.com, or:
- **Command-line only**, if not using Android Studio:
  - Android SDK command-line tools (`cmdline-tools`) from
    developer.android.com/tools, unzipped somewhere and `sdkmanager` on PATH.
  - Via `sdkmanager`: `platform-tools`, `platforms;android-34` (or latest),
    `build-tools;34.0.0`, and a system image if an emulator is wanted
    (`system-images;android-34;google_apis;x86_64`).
  - Gradle itself is not a separate install — the project's Gradle wrapper
    (`gradlew`) downloads the pinned Gradle version on first run; only the
    JDK is a real prerequisite for that.
- **A physical Android 12+ device with Bluetooth LE**, or an emulator image
  with BLE support enabled (BLE on emulators is limited/unreliable — a real
  phone is strongly preferred for this project, since the whole point is
  testing real BLE range/throughput to the ESP32).
- **USB debugging enabled** on the test phone (Settings > Developer options),
  plus `adb` (comes with `platform-tools`) to install/debug from the command
  line if not using Android Studio's device deployment.
- Nothing needs installing in this Linux environment for the "source only"
  workflow the user chose — the app is written as plain Kotlin/Gradle files
  in the repo and built/run by the user in Android Studio.
