# Audio and AI tests

## Prerecorded topic replies

The firmware streams recordings over BLE to the Android relay, which sends
them directly to Gemini. Gemini picks one
topic from `backend/topics.json`; the device plays that answer from LittleFS.
With the phone, off-topic questions play a random fallback; noise plays nothing. The app has
no TTS: all speech is prerecorded. To test clips without the network, send
`speech 12` (clips/012.wav), `speech off_3`, or `speech` (random) on serial.

Boot enables five minutes of offline activity. Without a phone, completed voice
or KEY1 captures of at least 0.25 seconds play a random `off_*.wav` clip. Each
played response restarts the five-minute window; noise may also trigger the energy
detector offline. Window expiry turns off eyes/audio, followed by reset-only deep
sleep after one idle hour. Hardware validation of these transitions is pending.

1. Edit `backend/topics.json`: `id` (1–999), `topic` (description Gemini matches
   against; English is fine) and `answer` (Ukrainian text to voice), plus `fallbacks`.
2. Produce audio externally (any ffmpeg format), name it `idN.*` (topic N) or
   `fbN.*` (fallback N) in `phrases/`, and run `python3 backend/convert_phrases.py`.
   It writes 16 kHz mono IMA ADPCM `data/clips/NNN.wav` and `off_K.wav`, trims edge silence
   (`--max-pause=0.4` also shortens inner pauses), normalizes loudness, rejects
   clips over 10 s, lists missing clips, reports LittleFS usage, and regenerates the
   Android topic asset (descriptions and fallback count only; answers stay local).
3. Rebuild/install the Android app, run PlatformIO *Upload Filesystem Image*, then
   upload the firmware.
   Regenerate old 24 kHz clips before uploading; the decoder requires 16 kHz.
   Clip playback uses 16 kHz; recording and microphone loopback remain 16 kHz.
4. Expect `AI reply: <answer>` and `Speech timing: clip=NNN, ..., total=...` (ms
   since recording ended). A missing clip prints `Speech: /clips/... missing`.

Improve a misclassified question by rewording topic descriptions. Tests:
`gradle :app:testDebugUnitTest` in the Android project, and
`g++ -std=c++11 -Wall -Wextra -Werror -Iinclude src/speech_clip.cpp test/speech_clip_test.cpp -o /tmp/speech_clip_test && /tmp/speech_clip_test`.

## Manual playback from Android

Install the updated Android app and firmware. Connect, pair, and wait for Ready.
Enter `1`, `001`, or `off_1` in the ESP32 clip field and press **Play**. The clip
must already exist in LittleFS; no Gemini API key or internet is needed.
Only upload the filesystem if the desired clips are missing or changed.
The app confirms the BLE write; check serial for missing files or busy requests.
Manual commands never replace an in-flight voice reply. See [BLE.md](BLE.md).

## Microphone to AI voice reply

Install the current Android app, enter your Gemini API key, and connect it to
Parrot. Upload the filesystem image and firmware when needed for clip changes.
Gemini classifies understandable speech by topic; silence or unclear speech should
produce `ignore`. Verify this with real recordings; there is no generated answer
or `/ask` endpoint in the new path.

1. Connect and pair the Android relay, subscribe to voice notifications with
   MTU 185 (see [BLE.md](BLE.md)), and open serial at 115200 baud.
2. Wait one second, then speak a short question without pressing KEY1.
   Recording stops after 800 ms of silence or at five seconds, including pre-roll.
3. Expect the relay to return a clip choice and the device to play it.
   Listen for the matching clip and inspect serial for playback errors.
4. Verify a relevant clip and continued eye animation.

Voice activation keeps up to 200 ms of pre-roll and requires 80 ms above an
adaptive sound-level threshold. It pauses during requests and playback, while
offline, and for one second afterward. Loud background sounds can trigger it;
this is not a wake-word or speech recognition detector. In `include/config.h`,
raise `AUDIO_VAD_MIN_RMS` if it triggers in silence, or lower it if speech is missed.
`AUDIO_VOICE_ACTIVATION=0` restores button-only activation. KEY1 remains available
to start manually; release ends the recording, with the same silence/time limits.

`AUDIO_AI_REPLY_TEST` in `include/config.h` defaults to 1, so recordings
upload instead of replaying. Set it to 0 and rebuild to restore local replay.
`AUDIO_AI_MIC_CHANNEL` selects channel 0 or 1 from the stereo microphone capture.
The other channel is discarded, avoiding phase cancellation from mixing mics.
If responses repeatedly report unclear speech, verify local replay and try the
other channel. The Android relay accepts 0.25-30 seconds; firmware records at most five
seconds, controlled by `AUDIO_RECORD_SECONDS`.

For hardware validation, check that quiet does not trigger, speech starts capture,
an 800 ms pause stops it, and continuous speech stops at five seconds. Check that
playback does not trigger a recording and KEY1 still works.

BLE streaming starts with recording. The Android relay decodes the audio and
submits an inline WAV to Gemini after capture ends. See [BLE.md](BLE.md) for the
packet contract, short-recording cancellation and disconnect behavior.
Only one recording/request or pending speech response is allowed at a time.
Each question is independent, with no conversation history. Recorded audio goes
through the relay to Gemini; the app does not persist audio or log request bodies.

The relay validates the sample rate, duration and decoded byte count before
contacting Gemini. Verify real-audio results and provider errors on a phone.

## Measure response latency

Measure from recording end until audible playback, and repeat the same short
question three times. Inspect `Speech timing:` on serial and relay status
for their respective stages. Firmware no longer owns an HTTP connection or
reports HTTP connection reuse, keep-alive or NTP timing.

Check disconnect/reconnect during capture and reply waiting, then pair and
subscribe again before another question. These checks require hardware; a build
cannot establish end-to-end latency or BLE reliability.

## Gemini API key

Add the key in Android Settings; it is saved in private internal storage. No device token or
Cloudflare deployment is needed. Firmware has no provider key, HTTPS certificate
or NTP synchronization. The previous Cloudflare deployment was not changed.
