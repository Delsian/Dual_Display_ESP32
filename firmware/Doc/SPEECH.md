# Audio and AI tests

## Prerecorded topic replies

The firmware sends recordings to `/intent`. Gemini picks one
topic from `backend/topics.json`; the device plays that answer from LittleFS.
Off-topic questions play a random fallback; noise plays nothing. The Worker has
no TTS: all speech is prerecorded. To test clips without the network, send
`speech 12` (clips/012.wav), `speech off_3`, or `speech` (random) on serial.

1. Edit `backend/topics.json`: `id` (1–999), `topic` (description Gemini matches
   against; English is fine) and `answer` (Ukrainian text to voice), plus `fallbacks`.
2. Produce audio externally (any ffmpeg format), name it `idN.*` (topic N) or
   `fbN.*` (fallback N) in `phrases/`, and run `python3 backend/convert_phrases.py`.
   It writes 24 kHz mono IMA ADPCM `data/clips/NNN.wav` and `off_K.wav`, trims edge silence
   (`--max-pause=0.4` also shortens inner pauses), normalizes loudness, rejects
   clips over 10 s, lists missing clips, reports LittleFS usage, and regenerates the
   Worker topic block (descriptions and fallback count only; answers stay local).
3. Deploy `backend/worker.mjs`, run PlatformIO *Upload Filesystem Image*, then
   upload the firmware.
   Regenerate old 16 kHz clips before uploading; the decoder requires 24 kHz.
   Clip playback uses 24 kHz; recording and microphone loopback remain 16 kHz.
4. Expect `AI reply: <answer>` and `Speech timing: clip=NNN, ..., total=...` (ms
   since recording ended). A missing clip prints `Speech: /clips/... missing`.

Improve a misclassified question by rewording topic descriptions. Tests:
`node --test test/worker_test.mjs test/worker_intent_test.mjs`, and
`g++ -std=c++11 -Wall -Wextra -Werror -Iinclude src/speech_clip.cpp test/speech_clip_test.cpp -o /tmp/speech_clip_test && /tmp/speech_clip_test`.

## Microphone to AI voice reply

Deploy the current `backend/worker.mjs` first; it provides protected `/intent`,
`/ask` and `/test-ai` (no TTS). Upload the filesystem image and firmware.

`/ask` system instructions allow English and Ukrainian replies only.
Unrecognized or absent speech is ignored: Gemini's `[IGNORE]` marker becomes
`{ok:true, text:"", ignored:true}`, so existing firmware skips voice output.
Recognition remains model-based; verify with silence/noise and a clear question.
Other languages receive an English request to use a supported language. These are model
instructions, not a strict output-language validator. After changing the local
API key, also update the deployed Worker's `GEMINI_API_KEY` secret in Cloudflare;
`.dev.vars` changes do not update the deployed service. Deploy `backend/worker.mjs`
to apply instruction changes, then test both allowed languages and another language.

1. Wait for Wi-Fi to connect and open serial at 115200 baud.
2. Wait one second, then speak a short question without pressing KEY1.
   Recording stops after 800 ms of silence or at five seconds, including pre-roll.
3. Expect `AI: HTTP 200`, `AI reply: <topic description>`, `Speech timing: clip=...`,
   then `Speech: playing AI voice.` Listen for the matching clip.
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
other channel. The Worker accepts 0.25-30 seconds; firmware records at most five
seconds, controlled by `AUDIO_RECORD_SECONDS`.

For hardware validation, check that quiet does not trigger, speech starts capture,
an 800 ms pause stops it, and continuous speech stops at five seconds. Check that
playback does not trigger a recording and KEY1 still works.

Upload starts when recording starts. The network task sends the selected channel
from the recording buffer as 128 ms HTTP chunks of
`audio/L16; rate=16000; channels=1; endianness=little-endian`. The final chunk
follows the silence cutoff, so connection setup and upload overlap speech. The
Worker adds the WAV header; `/ask` and `/intent` also accept `audio/wav`.
A recording under 0.25 s, or a failed write, closes the socket before the final
chunk, so the Worker rejects the body without calling Gemini.
A new recording cannot overwrite an in-flight upload. Only one network request
or pending speech response is allowed at a time; a busy request is rejected and
is not sent later. Errors are reported on serial; retry manually. Each question
is independent, with no conversation history. Recorded audio goes through the
Worker to Gemini. The Worker does not persist it or log request bodies.

The Worker rejects wrong content types, malformed WAV headers and oversized
uploads before contacting Gemini. Its free-plan CPU budget still needs validation
with real recordings, especially near the 30-second limit.

## Measure response latency

`Speech timing: clip=... total=` is the time from the end of recording until the
clip is decoded and handed to the audio task, the main latency figure.
`AI Worker timing:` reports `gemini_headers`, `gemini_body`, `upload_prepare`,
`gemini` and `worker_total`. Provider header timing includes networking and
queueing, not only inference. Worker timings overlap device timings; do not add them.

Deploy the updated Worker and upload firmware, then ask the same short question
three times within a minute. Copy the `HTTP timing:`, `AI timing:` and
`AI Worker timing:` lines from serial.
The first request may include NTP clock synchronization, so keep it separate
from subsequent requests when comparing results.

- `task_wait`, `network_ready`: measured from recording start (streaming upload).
- `upload_tail`: recording end until the final chunk was written; near zero
  means the upload kept up with recording. `total` counts from recording end.
- HTTP `connection`: DNS, TCP and certificate-validated TLS setup. `reused=yes`
  means an existing socket was available before this check; its time should
  usually be near zero. A peer can still close the socket immediately afterward.
- `post_to_headers`: sending the WAV, Worker processing and waiting for response
  headers. It normally excludes the separately measured connection setup; the
  HTTP library may reconnect if it detects closure between the check and POST.
- `response_body`: time after headers until the reply body has been read,
  including local response buffer allocation and the HTTP status log.
- `parse`: connection cleanup and parsing/validating the reply JSON.
- `total`: elapsed time from the upload request when recording stops until the reply
  is ready to print; excludes the recording itself and serial reply printing.
- Worker `upload_prepare`: receiving, validating and base64-encoding the WAV
  after the Worker handler begins. With streaming upload it includes most of the
  recording duration, which is expected.
- Worker `gemini`: sending the Gemini request and receiving/parsing its full
  response, including network time, not just model inference.
- Worker `worker_total`: time in the handler until preparing the JSON response.

All values are elapsed milliseconds. Worker durations overlap the ESP32 request
duration; do not add them together. Worker clocks can have coarse resolution
between I/O events, so small processing times are approximate. An older Worker
still works but the firmware reports that Worker timings are unavailable.

One persistent network task owns the HTTPS client for Worker requests. Complete responses allow HTTP keep-alive reuse. While idle, it
sends `GET /health` on an existing connection every 25 seconds and consumes the
response. These requests do not call Gemini or extend the inactivity window.
The connection closes after three minutes since the last user network job
completed, or on Wi-Fi loss. Failed/partial responses also close the socket.
Background pings do not reopen a closed connection; the next user request does.
Health reads use a three-second timeout; a queued request waits for an ongoing ping.
If the peer closes during a POST, the request fails rather than automatically
replaying a potentially processed AI request; retry manually.

Network jobs leave Wi-Fi power saving unchanged. Modem sleep must remain enabled
while BLE is active; disabling it causes the Wi-Fi driver to abort on this board.
HTTPS connection reuse and certificate validation remain enabled.

After flashing, check three consecutive questions for `reused=yes` after the
first, then wait 30–60 seconds and check keep-alive logs and connection reuse.
Wait over three minutes after a completed request: expect a close log, no further
pings, and a fresh connection on the next question. No Worker redeployment is needed.
Also test a Wi-Fi disconnect/reconnect followed by a question. These require hardware; a successful build cannot establish the
latency improvement or the server's connection retention behavior.

## Device token

The local, ignored `include/speech_secrets.h` contains `SPEECH_DEVICE_TOKEN`,
matching the Worker's `DEVICE_TOKEN`. Update it if the device token changes. The
token is embedded in the firmware binary; keep that binary private. The Gemini key
is never included in firmware. Without the local header, AI requests are disabled.
The network task checks HTTPS certificates using ISRG roots and synchronizes time
with NTP when needed.
