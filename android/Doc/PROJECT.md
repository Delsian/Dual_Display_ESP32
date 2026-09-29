# Android project context

## Goal and scope

Provide a native Android relay between the ESP32 Parrot and Gemini:
receive a recording over BLE, submit it for intent selection, and return a clip
choice for playback on the device. Keep Android-specific context here;
firmware internals belong to the sibling project.

## Architecture and source map

- [MainActivity.kt](../app/src/main/java/com/eugkrashtan/parrot/MainActivity.kt):
  programmatic UI, Settings with Free/Paid Save/Test controls, Bluetooth permission request,
  service Start/Stop controls, stored ESP32 clip input/Play, bound status and Parrot battery percentage.
- [RelayService.kt](../app/src/main/java/com/eugkrashtan/parrot/RelayService.kt):
  connected-device foreground service owns the relay, notification, saved-key access,
  and bounded request wake lock; activity closure leaves it running.
- [RelayBootReceiver.kt](../app/src/main/java/com/eugkrashtan/parrot/RelayBootReceiver.kt):
  resumes an enabled service after boot/app update when Bluetooth permissions exist.
- [ApiKeyStore.kt](../app/src/main/java/com/eugkrashtan/parrot/ApiKeyStore.kt):
  atomic key writes to app-private internal storage excluded from backup/transfer.
- [VoiceRelay.kt](../app/src/main/java/com/eugkrashtan/parrot/VoiceRelay.kt): BLE
  filtered low-power scanning, automatic GATT reconnects, setup/write timeouts,
  notification subscription, packetized IMA ADPCM
  decoding, direct Gemini classification, clip/ignore replies, and manual playback.
- [ClipSelection.kt](../app/src/main/java/com/eugkrashtan/parrot/ClipSelection.kt):
  validates local clip names and normalizes topic numbers before manual playback.
- [GeminiIntent.kt](../app/src/main/java/com/eugkrashtan/parrot/GeminiIntent.kt):
  inline WAV requests, enum topic classification, validated clip/ignore mapping.
  [Topic catalog](../app/src/main/assets/intent_topics.json) is generated from
  firmware `backend/topics.json`; rebuild Android after topic changes.
- [GeminiKeyFallback.kt](../app/src/main/java/com/eugkrashtan/parrot/GeminiKeyFallback.kt):
  retries free-key failures once with paid, then retries free after one hour.
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
  Firmware owns capture and local clip playback. Android calls Gemini to select the intent; Cloudflare is no longer required.
- Manual playback uses the shared BLE contract and requires updated firmware.
  Connecting and playing stored clips does not require an API key; the app
  confirms BLE delivery only, with playback errors available on firmware serial.
- Battery is read on connection and updated through notifications, separately from
  voice packets. The service retains the value across activity reopening and clears
  it on disconnect; unavailable/invalid values show a dash. BLE contract is unchanged.
- Start enables persistent background operation; Stop disables it. The service
  recovers after disconnection, Bluetooth toggles and eligible process/boot restarts.
  Firmware pairing still requires user approval where bonds are not retained.
  Android force-stop/battery policy can block restart; see [README](../README.md).
- The app submits inline WAV audio to Gemini and returns `clip:<name>` or `ignore`.
  It does not generate speech or play the reply itself.
- Settings saves separate free and optional paid keys for subsequent requests;
  the existing key remains free. Empty paid disables fallback. Any classification
  exception retries once with paid within the same 25-second deadline. Paid stays
  selected for one hour, then free is retried; paid failures do not extend this period.
  Key changes/service recreation reset selection to free. Per-key tests do not alter
  selection. Masked fields disable view-state saving/autofill; storage excludes backups.
  Keep credentials out of source, context, and logs.
- The former Worker model and classifier prompt are preserved. No speech generation
  or TTS is used. Requests are independent; no conversation history is sent.
- The local reply deadline is 25 seconds, below firmware's 30-second wait.
  Cancel/disconnect/new recordings suppress stale results; HTTP may finish later.
- Cross-project changes need a concise handoff describing contract impact,
  required counterpart changes, and separately verified results.

## Validation

Use Android Studio with JDK 17 and SDK 35 for Gradle sync and builds. Physical
phone/ESP32 checks are required for pairing, streaming, reconnects, audio, and
latency. See [STATUS.md](STATUS.md) for evidence and remaining work.
