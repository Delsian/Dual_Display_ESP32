# Parrot Relay for Android

Native Android 12+ companion app for the ESP32 Parrot BLE voice link. Open the `android/` directory in Android Studio, let Gradle sync, and run it on a physical Android device with Bluetooth LE.

## Setup

1. Install Android Studio with JDK 17 and Android SDK 35.
2. Open this directory as a project.
3. Run the app on a physical Android 12+ phone.
4. Grant the Nearby devices permission.
5. Open **Settings**, enter your Gemini API key, and press **Add** to save it.
   Press **Test** to check the entered key, close Settings, then **Scan and connect**.
6. Pair with the six-digit passkey shown on the ESP32 display when Android requests it.

The key is saved in app-private internal storage, excluded from backup/device transfer,
and loaded on app startup. **Add** replaces the saved key and applies it to subsequent
requests immediately. The field is masked; view-state saving and autofill stay disabled.
**Test** checks the entered value without saving it, using a small request to the same
Gemini model as the relay. This uses API quota. Results distinguish access, quota,
network, and response failures; a failed test does not overwrite the saved key.
The app calls Gemini directly; Cloudflare and a device token are no longer required.

## Play a stored ESP32 clip

Connect and wait for **Ready**, enter a clip number or name (for example `1`,
`001`, or `off_1`) in the main screen's clip field, then press **Play**.
Numbers 1–999 are padded to three digits. Do not include `.wav` or a path.
The clip plays on the ESP32 speaker; no API key or internet connection is needed.
Gemini voice classification still needs a saved key.

This requires the updated firmware's manual-play command. Clips must already
be in the ESP32 filesystem. The app reports the BLE write result; it does not
receive playback completion or missing-file status. Check firmware serial logs
for those errors. Wait for a recording/request to finish before manual playback.

## Relay behavior

The app scans for `Parrot-XXXXXX`, negotiates MTU 185, subscribes to the voice TX characteristic, decodes the firmware's packetized IMA ADPCM to 16-bit mono PCM, sends an inline base64 WAV to Gemini, and writes `clip:<name>` or `ignore` back to the ESP32.

Protocol and hardware behavior are documented in [the shared BLE contract](../firmware/Doc/BLE.md). BLE throughput, pairing, reconnects, and end-to-end audio still require physical-device validation.

## Intent selection and validation

`GeminiIntent.kt` preserves the former Worker's model (`gemini-3.6-flash`),
prompt, enum response schema, and topic/fallback/ignore behavior. Unknown,
blocked, empty, or incomplete results return an error instead of playing a clip.
Requests time out locally after 25 seconds; cancelled, disconnected, superseded,
and late results cannot produce a reply. Network work may finish after cancellation.

The catalog in `app/src/main/assets/intent_topics.json` contains descriptions only.
Its source is `../firmware/backend/topics.json`. After editing that source, run
`python3 backend/convert_phrases.py --topics-only` from the firmware directory,
then rebuild this app. Clip conversion also refreshes the catalog.

Run `gradle :app:testDebugUnitTest :app:assembleDebug` with the installed Gradle.
Tests do not contact Gemini. Live key/model access, quota failures, classification,
cancellation, and phone/ESP32 playback still need device validation.
The old Cloudflare deployment has not been changed or deleted.

## Project context

Start Android agent sessions in this directory. [AGENTS.md](AGENTS.md) defines
the working rules; [Doc/PROJECT.md](Doc/PROJECT.md) describes the local
architecture, and [Doc/STATUS.md](Doc/STATUS.md) tracks implementation and
validation evidence. Read firmware documentation only when a task involves
device integration; use the shared BLE contract rather than duplicating it.
