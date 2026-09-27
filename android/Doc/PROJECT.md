# Android project context

## Goal and scope

Provide a native Android relay between the ESP32 Parrot and its cloud Worker:
receive a recording over BLE, submit it for intent selection, and return a clip
choice for playback on the device. Keep Android-specific context here;
firmware internals belong to the sibling project.

## Architecture and source map

- [MainActivity.kt](../app/src/main/java/com/eugkrashtan/parrot/MainActivity.kt):
  programmatic UI, endpoint/token input, Bluetooth permission request,
  connection controls, and status display.
- [VoiceRelay.kt](../app/src/main/java/com/eugkrashtan/parrot/VoiceRelay.kt): BLE
  scanning and GATT connection, notification subscription, packetized IMA ADPCM
  decoding, PCM WAV upload over HTTPS, and clip/ignore replies.
- [App configuration](../app/build.gradle.kts): Kotlin Android app, minimum
  SDK 31 (Android 12), compile/target SDK 35, Java/Kotlin target 17.
- [Manifest](../app/src/main/AndroidManifest.xml): Bluetooth scan/connect and
  Internet permissions; launcher activity.
- [README.md](../README.md): setup and manual device workflow.

## Integration boundaries

- The authoritative shared BLE contract is
  [firmware/Doc/BLE.md](../../firmware/Doc/BLE.md). Read it for protocol tasks;
  do not duplicate UUIDs, packet layouts, or protocol rules in this summary.
- Android owns the relay UI, permissions, BLE client, decoding, and HTTP client.
  Firmware owns capture and local clip playback. The Worker selects the intent.
- The app submits WAV audio to `/intent` and returns `clip:<name>` or `ignore`.
  It does not generate speech or play the reply itself.
- Device credentials are entered at runtime. Keep credentials out of source,
  context, and logs; provider credentials belong to the backend.
- Cross-project changes need a concise handoff describing contract impact,
  required counterpart changes, and separately verified results.

## Validation

Use Android Studio with JDK 17 and SDK 35 for Gradle sync and builds. Physical
phone/ESP32 checks are required for pairing, streaming, reconnects, audio, and
latency. See [STATUS.md](STATUS.md) for evidence and remaining work.
