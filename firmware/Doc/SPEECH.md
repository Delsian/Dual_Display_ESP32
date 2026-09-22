# Audio and AI tests

## Microphone to AI voice reply

Deploy the current `backend/worker.mjs` first; it provides protected `/ask` and `/speak`.
Then upload the firmware normally, without uploading the filesystem.

`/ask` system instructions allow English and Ukrainian replies only. Other
languages receive an English request to use a supported language. These are model
instructions, not a strict output-language validator. After changing the local
API key, also update the deployed Worker's `GEMINI_API_KEY` secret in Cloudflare;
`.dev.vars` changes do not update the deployed service. Deploy `backend/worker.mjs`
to apply instruction changes, then test both allowed languages and another language.

1. Wait for Wi-Fi to connect and open serial at 115200 baud.
2. Wait one second, then speak a short question without pressing KEY1.
   Recording stops after 800 ms of silence or at five seconds, including pre-roll.
3. Expect `AI: uploading ... ms of microphone audio.`, `AI: HTTP 200`, then
   `AI reply: ...`, `Speech: requesting AI reply voice.`, `Speech: HTTP 200`,
   and `Speech: playing AI voice.` Listen for the actual answer.
4. Verify a relevant reply, continued eye animation, and the `speech` command.

The firmware sends the printed answer to `/speak` as JSON text. This adds a TTS
provider call and generation/download latency after the text reply. The existing
voice model, Kore voice, and ten-second WAV limit are reused. Replies are prompted
to stay within 20 words; oversized or invalid generated audio is rejected, not
truncated. If TTS fails, the serial text remains available and capture resumes.
Test English and Ukrainian questions, no self-triggering during speaker playback,
and the ability to ask another question after the cooldown. Timing labeled `AI`
still measures the text reply only, excluding subsequent speech generation.

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
playback does not trigger a recording and KEY1 still works. Only a firmware upload
is needed for this change; the Worker and filesystem are unchanged. Upload still
begins after recording stops.

The recording is copied into a mono 16 kHz PCM WAV before the upload task starts.
A new recording cannot overwrite an in-flight upload. Only one network request
or pending speech response is allowed at a time; a busy request is rejected and
is not sent later. Errors are reported on serial; retry manually. Each question
is independent, with no conversation history. Recorded audio goes through the
Worker to Gemini. The Worker does not persist it or log request bodies.

The Worker rejects wrong content types, malformed WAV headers and oversized
uploads before contacting Gemini. Its free-plan CPU budget still needs validation
with real recordings, especially near the 30-second limit.

## Measure response latency

Deploy the updated Worker and upload firmware, then ask the same short question
three times within a minute. Copy the `HTTP timing:`, `AI timing:` and
`AI Worker timing:` lines from serial.
The first request may include NTP clock synchronization, so keep it separate
from subsequent requests when comparing results.

- `prepare`: local WAV allocation and microphone channel extraction.
- `task_wait`: time until the background upload task starts.
- `network_ready`: Wi-Fi check and any clock synchronization.
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
  after the Worker handler begins; excludes any earlier network buffering.
- Worker `gemini`: sending the Gemini request and receiving/parsing its full
  response, including network time, not just model inference.
- Worker `worker_total`: time in the handler until preparing the JSON response.

All values are elapsed milliseconds. Worker durations overlap the ESP32 request
duration; do not add them together. Worker clocks can have coarse resolution
between I/O events, so small processing times are approximate. An older Worker
still works but the firmware reports that Worker timings are unavailable.

One persistent network task owns the HTTPS client for both `/ask` and
`/test-speech`. Complete responses allow HTTP keep-alive reuse. Failed requests
and partial/unread responses close the socket; idle connections close after
about 60 seconds. A disconnected socket is re-established on the next request.
If the peer closes during a POST, the request fails rather than automatically
replaying a potentially processed AI request; retry manually.

Network jobs leave Wi-Fi power saving unchanged. Modem sleep must remain enabled
while BLE is active; disabling it causes the Wi-Fi driver to abort on this board.
HTTPS connection reuse and certificate validation remain enabled.

After flashing, check three consecutive questions for `reused=yes` after the
first, compare total latency, then wait over a minute and check a fresh connection.
Also test a Wi-Fi disconnect/reconnect and the `speech` command followed by a
question. These require hardware; a successful build cannot establish the
latency improvement or the server's connection retention behavior.

## Fixed-phrase voice playback test

The deployed Worker must provide the authenticated `POST /test-speech` endpoint.
It returns a WAV of the AI-generated phrase "Parrot is ready."

The local, ignored `include/speech_secrets.h` contains `SPEECH_DEVICE_TOKEN`,
matching the Worker's `DEVICE_TOKEN`. It was populated from `.dev.vars` for this
checkout. Update this header too if the device token changes. The device token
is embedded in the firmware binary; keep that binary private. The Gemini key
is never included in firmware. Without the local header, the speech test is disabled.

1. Build and upload firmware (no filesystem upload is needed):
   `pio run -e esp32-s3-dualeye-touch-lcd-1_28 -t upload`
2. Open the serial monitor at 115200 baud and wait for Wi-Fi to connect.
3. Send `speech` followed by Enter.
4. Expect `Speech: HTTP 200`, then `Speech: playing AI voice.` and
   `Audio: playback complete.` Listen for the spoken phrase at normal speed.
5. Check eye animation continues during the request. Pressing KEY1 interrupts
   speech playback and starts recording for the configured KEY1 mode.

The network task checks HTTPS certificates using ISRG roots and synchronizes time
with NTP when needed. A bounded PSRAM buffer holds up to ten seconds of WAV audio.
The response is low-pass filtered and resampled from 24 kHz mono to the existing
16 kHz stereo I2S format. Playback waits until microphone recording/replay is idle.
Only one download/pending response is allowed; repeated commands report busy.

An offline request or HTTP failure leaves the audio task available.
An invalid token returns HTTP 401; quota exhaustion returns HTTP 429. A clock
sync failure is reported without disabling certificate validation. Send `speech`
again after resolving the problem; requests are not automatically retried.

Host validation of WAV rejection, channel duplication, duration, and anti-alias
filtering:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -Iinclude src/speech_wav.cpp test/speech_wav_test.cpp -o /tmp/speech_wav_test
/tmp/speech_wav_test
```

The `speech` command tests fixed speech playback only and does not upload audio.
