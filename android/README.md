# Parrot Relay for Android

Native Android 12+ companion app for the ESP32 Parrot BLE voice link. Open the `android/` directory in Android Studio, let Gradle sync, and run it on a physical Android device with Bluetooth LE.

## Setup

1. Install Android Studio with JDK 17 and Android SDK 35.
2. Open this directory as a project.
3. Run the app on a physical Android 12+ phone.
4. Grant the Nearby devices permission.
5. Enter the Worker device token and press **Scan and connect**.
6. Pair with the six-digit passkey shown on the ESP32 display when Android requests it.

The default Worker URL is `https://parrot.eug-krashtan.workers.dev/intent`. The token is entered at runtime and is not stored in source control.

## Relay behavior

The app scans for `Parrot-XXXXXX`, negotiates MTU 185, subscribes to the voice TX characteristic, decodes the firmware's packetized IMA ADPCM to 16-bit mono PCM, posts a WAV to `/intent`, and writes `clip:<name>` or `ignore` back to the ESP32.

Protocol and hardware behavior are documented in [the shared BLE contract](../firmware/Doc/BLE.md). BLE throughput, pairing, reconnects, and end-to-end audio still require physical-device validation.

## Project context

Start Android agent sessions in this directory. [AGENTS.md](AGENTS.md) defines
the working rules; [Doc/PROJECT.md](Doc/PROJECT.md) describes the local
architecture, and [Doc/STATUS.md](Doc/STATUS.md) tracks implementation and
validation evidence. Read firmware documentation only when a task involves
device integration; use the shared BLE contract rather than duplicating it.
