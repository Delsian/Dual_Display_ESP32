#include <Arduino.h>
#include "config.h"
#include "speech_test.h"

#if USE_AUDIO
#include "speech_clip.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <memory>
#include <new>
#if __has_include("speech_secrets.h")
#define SPEECH_NETWORK 1
#include "speech_secrets.h"
#include "speech_ca.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#else
#define SPEECH_NETWORK 0 // Local clip playback still works without the token.
#endif

struct SpeechAudio {
  explicit SpeechAudio(int16_t *data) : pcm(data) {}
  int16_t *pcm;
  std::atomic<size_t> bytes{0};
  std::atomic<bool> done{false};
  std::atomic<int> owners{2};  // Network and audio tasks; the last one frees.
  std::atomic<int> handoff{2}; // Download ended and playback taken.
};

namespace {
constexpr size_t MAX_SPEECH_FRAMES = SPEECH_CLIP_SAMPLE_RATE * 10; // Ten seconds.
std::atomic<bool> busy{false};
std::atomic<SpeechAudio *> ready{nullptr};

void release(SpeechAudio *audio) {
  if (audio->owners.fetch_sub(1) != 1) return;
  heap_caps_free(audio->pcm);
  delete audio;
}

// Busy clears only after the download ends and the audio task takes the reply,
// so a pending reply is never replaced.
void hand_off(SpeechAudio *audio) {
  if (audio->handoff.fetch_sub(1) == 1) busy.store(false);
}

// Decodes a prerecorded reply from LittleFS and hands it to the audio task.
bool play_clip(const char *name, uint32_t started) {
  constexpr size_t MAX_CLIP_BYTES = 128 * 1024; // Ten seconds of 24 kHz ADPCM plus headers.
  if (!speech_clip_name_valid(name)) { Serial.println("Speech: invalid clip name."); return false; }
  char path[32];
  snprintf(path, sizeof(path), "/clips/%s.wav", name);
  const uint32_t load_started = millis();
  File file = LittleFS.open(path, "r");
  const size_t bytes = file ? file.size() : 0;
  if (!bytes || bytes > MAX_CLIP_BYTES) {
    Serial.printf("Speech: %s missing or too large; upload the filesystem image.\n", path);
    return false;
  }
  std::unique_ptr<uint8_t, void (*)(void *)> wav(
      static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
  const bool loaded = wav && file.read(wav.get(), bytes) == bytes;
  file.close();
  const size_t frames = loaded ? speech_clip_frames(wav.get(), bytes, MAX_SPEECH_FRAMES) : 0;
  auto *pcm = frames ? static_cast<int16_t *>(heap_caps_malloc(frames * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr;
  auto *audio = pcm ? new (std::nothrow) SpeechAudio(pcm) : nullptr;
  if (!audio || !speech_clip_decode(wav.get(), bytes, pcm, frames)) {
    delete audio;
    heap_caps_free(pcm);
    Serial.printf("Speech: cannot load %s (%s).\n", path, frames ? "memory or decode" : "invalid clip");
    return false;
  }
  audio->bytes.store(frames * 4);
  audio->done.store(true);
  ready.store(audio);
  Serial.printf("Speech timing: clip=%s, frames=%u, load=%lu ms, total=%lu ms\n", name, unsigned(frames),
                static_cast<unsigned long>(millis() - load_started),
                static_cast<unsigned long>(millis() - started));
  hand_off(audio);
  release(audio);
  return true;
}

// Picks a random clip name from /clips; returns false if none exist.
bool random_clip(char *name, size_t size) {
  File dir = LittleFS.open("/clips");
  size_t count = 0;
  for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
    const String file = f.name();
    // Reservoir sampling: one pass, uniform choice without storing names.
    if (!file.endsWith(".wav") || file.length() - 4 >= size || random(++count) != 0) continue;
    snprintf(name, size, "%.*s", int(file.length() - 4), file.c_str());
  }
  return count > 0;
}

#if SPEECH_NETWORK
constexpr uint32_t KEEP_ALIVE_MS = 25000;
constexpr uint32_t CONNECTION_IDLE_MS = 180000;

class PersistentHttp : public HTTPClient {
 public:
  bool establish() { return connect(); }
  // Chunked uploads write the body directly but reuse the library's framing.
  bool send_request_header(const char *type) {
    for (size_t i = 0; i < _headerKeysCount; ++i) _currentHeaders[i].value.clear();
    return sendHeader(type);
  }
  int read_response() { return handleHeaderResponse(); }
};

// Accessed only by the persistent network task.
struct HttpSession {
  WiFiClientSecure tls;
  PersistentHttp http;
  uint32_t last_used = 0;

  HttpSession() {
    tls.setCACert(SPEECH_ROOT_CA);
    tls.setHandshakeTimeout(15);
    http.setConnectTimeout(15000);
    http.setTimeout(50000);
    http.setUserAgent("Parrot-ESP32/1.0");
  }
  void finish(bool complete) {
    http.setReuse(complete);
    http.end();
    if (!complete) tls.stop(); // Never reuse an unread or partial response.
    last_used = millis();
  }
  bool begin(const char *url) {
    http.setReuse(true);
    if (!http.begin(tls, url)) { finish(false); return false; }
    const bool reused = tls.connected();
    const uint32_t started = millis();
    const bool connected = http.establish(); // DNS + TCP + validated TLS, no POST yet.
    Serial.printf("HTTP timing: connection=%lu ms, reused=%s, connected=%s\n",
                  static_cast<unsigned long>(millis() - started),
                  reused ? "yes" : "no", connected ? "yes" : "no");
    if (!connected) finish(false);
    return connected;
  }
};

class WavSink : public Stream {
 public:
  uint8_t *buffer;
  size_t capacity;
  size_t used = 0;
  uint32_t started = millis();
  WavSink(uint8_t *data, size_t limit) : buffer(data), capacity(limit) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t *data, size_t bytes) override {
    if (bytes > capacity - used || millis() - started > 60000UL) return 0;
    memcpy(buffer + used, data, bytes);
    used += bytes;
    return bytes;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

bool network_ready() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Speech: Wi-Fi is not connected.");
    return false;
  }
  if (time(nullptr) < 1700000000) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    const uint32_t started = millis();
    while (time(nullptr) < 1700000000 && millis() - started < 10000UL) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (time(nullptr) < 1700000000) {
      Serial.println("Speech: clock sync failed; HTTPS requires valid time.");
      return false;
    }
  }
  return true;
}

// The recording being captured; the audio task appends, the network task
// uploads. Only one exists at a time: busy stays set until final is seen.
struct UploadJob {
  const uint8_t *stereo = nullptr;
  std::atomic<size_t> bytes{0};
  std::atomic<bool> final{false};
  uint32_t started = 0; // Recording start.
  uint32_t stopped = 0; // Recording end; written before final.
};
UploadJob upload;
bool upload_active = false; // Audio task only.

void wait_for_recording(UploadJob *job) {
  while (!job->final.load()) vTaskDelay(pdMS_TO_TICKS(10));
}

// Sends the selected microphone channel as HTTP chunks while recording
// continues. Returns false if the connection failed.
bool send_recording(WiFiClientSecure &tls, UploadJob *job, size_t &sent) {
  constexpr size_t CHUNK_FRAMES = 2048; // 128 ms per chunk.
  static uint8_t chunk[8 + CHUNK_FRAMES * 2 + 2];
  for (;;) {
    const bool final = job->final.load(); // Before bytes, so the final length is seen.
    const size_t ready = job->bytes.load() - sent;
    if (ready < CHUNK_FRAMES * 4 && !(final && ready)) {
      if (final) return true;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const size_t frames = min(ready / 4, CHUNK_FRAMES);
    snprintf(reinterpret_cast<char *>(chunk), 9, "%06X\r\n", unsigned(frames * 2));
    const uint8_t *source = job->stereo + sent + AUDIO_AI_MIC_CHANNEL * 2;
    for (size_t i = 0; i < frames; ++i) {
      chunk[8 + i * 2] = source[i * 4];
      chunk[9 + i * 2] = source[i * 4 + 1];
    }
    memcpy(chunk + 8 + frames * 2, "\r\n", 2);
    const size_t length = 10 + frames * 2;
    if (tls.write(chunk, length) != length) return false;
    sent += frames * 4;
  }
}

// Streams one recording to /intent while it is captured; the Worker picks a
// prerecorded clip. Returns true once clip audio reached the audio task.
bool reply_to_recording(HttpSession &session, UploadJob *job) {
  const uint32_t task_started = millis();
  auto fail = [&](const char *message) {
    session.finish(false); // Closing mid-body cancels the upload before Gemini.
    wait_for_recording(job);
    Serial.println(message);
    return false;
  };
  if (!network_ready()) return fail("AI: recording not sent.");
  const uint32_t network_ready_at = millis();
  Serial.printf("AI timing: task_wait=%lu ms, network_ready=%lu ms (after recording start)\n",
                static_cast<unsigned long>(task_started - job->started),
                static_cast<unsigned long>(network_ready_at - job->started));
  auto &http = session.http;
  if (!session.begin("https://parrot.eug-krashtan.workers.dev/intent")) return fail("AI: connection failed.");
  http.addHeader("Authorization", String("Bearer ") + SPEECH_DEVICE_TOKEN);
  http.addHeader("Content-Type", "audio/L16; rate=16000; channels=1; endianness=little-endian");
  http.addHeader("Transfer-Encoding", "chunked");
  const char *headers[] = {"Content-Type"};
  http.collectHeaders(headers, 1);
  size_t sent = 0;
  if (!http.send_request_header("POST") || !send_recording(session.tls, job, sent)) {
    return fail("AI: upload failed; check Wi-Fi.");
  }
  if (sent < 16000) return fail("AI: recording under 0.25 seconds; upload cancelled.");
  if (session.tls.write(reinterpret_cast<const uint8_t *>("0\r\n\r\n"), 5) != 5) {
    return fail("AI: upload failed; check Wi-Fi.");
  }
  const uint32_t request_started = millis();
  const int status = http.read_response();
  const uint32_t headers_at = millis();
  // upload_tail: recording end until the last chunk was written.
  Serial.printf("AI timing: upload_tail=%lu ms, post_to_headers=%lu ms, audio=%u ms\n",
                static_cast<unsigned long>(request_started - job->stopped),
                static_cast<unsigned long>(headers_at - request_started), unsigned(sent / 64));
  Serial.printf("AI: HTTP %d\n", status);
  constexpr size_t MAX_REPLY_BYTES = 8192;
  if (status != 200 || !http.header("Content-Type").startsWith("application/json") ||
      http.getSize() > static_cast<int>(MAX_REPLY_BYTES)) {
    session.finish(false);
    Serial.println("AI: request failed; check Worker deployment, token, Wi-Fi, or quota.");
    return false;
  }
  std::unique_ptr<uint8_t[]> reply(new (std::nothrow) uint8_t[MAX_REPLY_BYTES]);
  if (!reply) { session.finish(false); Serial.println("AI: response allocation failed."); return false; }
  WavSink sink(reply.get(), MAX_REPLY_BYTES);
  const int received = http.writeToStream(&sink);
  const uint32_t body_at = millis();
  session.finish(received > 0 && static_cast<size_t>(received) == sink.used);
  if (received <= 0 || static_cast<size_t>(received) != sink.used) {
    Serial.println("AI: incomplete response.");
    return false;
  }
  DynamicJsonDocument result(12288);
  const auto error = deserializeJson(result, reply.get(), sink.used);
  if (error || result["ok"] != true || !result["text"].is<const char *>()) {
    Serial.println("AI: invalid response.");
    return false;
  }
  const uint32_t parsed_at = millis();
  Serial.printf("AI timing: response_body=%lu ms, parse=%lu ms, total=%lu ms\n",
                static_cast<unsigned long>(body_at - headers_at),
                static_cast<unsigned long>(parsed_at - body_at),
                static_cast<unsigned long>(parsed_at - job->stopped));
  const JsonObjectConst timing = result["timing_ms"].as<JsonObjectConst>();
  if (timing["gemini_headers_ms"].is<uint32_t>() && timing["gemini_body_ms"].is<uint32_t>()) {
    Serial.printf("AI Worker timing: gemini_headers=%lu ms, gemini_body=%lu ms\n",
                  static_cast<unsigned long>(timing["gemini_headers_ms"].as<uint32_t>()),
                  static_cast<unsigned long>(timing["gemini_body_ms"].as<uint32_t>()));
  }
  if (timing["upload_prepare_ms"].is<uint32_t>() && timing["gemini_ms"].is<uint32_t>() &&
      timing["worker_total_ms"].is<uint32_t>()) {
    Serial.printf("AI Worker timing: upload_prepare=%lu ms, gemini=%lu ms, worker_total=%lu ms\n",
                  static_cast<unsigned long>(timing["upload_prepare_ms"].as<uint32_t>()),
                  static_cast<unsigned long>(timing["gemini_ms"].as<uint32_t>()),
                  static_cast<unsigned long>(timing["worker_total_ms"].as<uint32_t>()));
  } else {
    Serial.println("AI timing: Worker timings unavailable; deploy the updated Worker.");
  }
  Serial.print("AI reply: ");
  Serial.println(result["text"].as<const char *>());
  // Ignored speech has an empty clip and plays nothing.
  const char *clip = result["clip"];
  return clip && *clip && play_clip(clip, job->stopped);
}

QueueHandle_t requests = nullptr;

void keep_alive(HttpSession &session) {
  // Use the same client, and consume the whole response before allowing reuse.
  if (!session.begin("https://parrot.eug-krashtan.workers.dev/health")) return;
  auto &http = session.http;
  http.setTimeout(3000);
  const int status = http.GET();
  uint8_t body[256];
  WavSink sink(body, sizeof(body));
  int received = -1;
  if (status == 200 && http.getSize() <= static_cast<int>(sizeof(body))) {
    received = http.writeToStream(&sink);
  }
  const bool complete = received > 0 && static_cast<size_t>(received) == sink.used;
  session.finish(complete);
  http.setTimeout(50000);
  Serial.printf("HTTP: keep-alive %s (HTTP %d).\n", complete ? "ok" : "failed", status);
}

void network_task(void *) {
  HttpSession session;
  uint32_t last_request_completed = 0;
  bool active_window = false;
  for (;;) {
    UploadJob *job = nullptr;
    if (xQueueReceive(requests, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
      if (!active_window) continue;
      if (WiFi.status() != WL_CONNECTED || millis() - last_request_completed >= CONNECTION_IDLE_MS) {
        session.finish(false);
        active_window = false;
        Serial.println("HTTP: connection closed (offline or 3 minutes idle).");
      } else if (millis() - session.last_used >= KEEP_ALIVE_MS && session.tls.connected()) {
        keep_alive(session);
      }
      continue;
    }
    // Keep modem sleep enabled: this firmware runs Wi-Fi and BLE together.
    if (WiFi.status() != WL_CONNECTED) session.finish(false);
    if (!reply_to_recording(session, job)) busy.store(false);
    // Health requests never extend this window; only completed user jobs do.
    last_request_completed = millis();
    active_window = true;
  }
}

// Called with busy held, so queue/task initialization cannot race.
bool enqueue(UploadJob *job) {
  if (!requests) {
    requests = xQueueCreate(1, sizeof(UploadJob *));
    if (!requests) return false;
    if (xTaskCreate(network_task, "voice_network", 8192, nullptr, 1, nullptr) != pdPASS) {
      vQueueDelete(requests);
      requests = nullptr;
      return false;
    }
  }
  return xQueueSend(requests, &job, 0) == pdTRUE;
}
#endif
}

// Serial "speech [N|off_K]": plays a clip from LittleFS without the network.
void request_speech_test(const char *clip) {
  char name[20];
  if (!clip || !*clip) {
    if (!random_clip(name, sizeof(name))) {
      Serial.println("Speech: no clips in /clips; upload the filesystem image.");
      return;
    }
  } else {
    char *end = nullptr;
    const long id = strtol(clip, &end, 10);
    if (*end == '\0' && id >= 1 && id <= 999) snprintf(name, sizeof(name), "%03ld", id);
    else snprintf(name, sizeof(name), "%s", clip);
  }
  if (busy.exchange(true)) { Serial.println("Speech: request already pending."); return; }
  Serial.printf("Speech: local clip %s.\n", name);
  if (!play_clip(name, millis())) busy.store(false);
}

#if SPEECH_NETWORK
bool audio_reply_available() {
  return WiFi.status() == WL_CONNECTED && !busy.load();
}

bool begin_audio_reply(const uint8_t *stereo) {
  static_assert(AUDIO_AI_MIC_CHANNEL <= 1 && AUDIO_AI_MIC_CHANNEL >= 0, "Invalid microphone channel");
  if (busy.exchange(true)) { Serial.println("AI: request already pending; recording not sent."); return false; }
  // Busy was clear, so the network task no longer reads the previous job.
  upload.stereo = stereo;
  upload.bytes.store(0);
  upload.final.store(false);
  upload.started = millis();
  if (!enqueue(&upload)) {
    busy.store(false);
    Serial.println("AI: task allocation failed.");
    return false;
  }
  upload_active = true;
  return true;
}

void audio_reply_progress(size_t bytes, bool final) {
  if (!upload_active) return;
  if (final) upload.stopped = millis();
  upload.bytes.store(bytes);
  if (!final) return;
  upload.final.store(true);
  upload_active = false;
  Serial.printf("AI: recorded %u ms; finishing upload.\n", unsigned(bytes / 64));
}

#else
bool audio_reply_available() { return false; }
bool begin_audio_reply(const uint8_t *) {
  Serial.println("AI: local device token is unavailable.");
  return false;
}
void audio_reply_progress(size_t, bool) {}
#endif

SpeechAudio *take_speech_audio() {
  SpeechAudio *audio = ready.exchange(nullptr);
  if (audio) hand_off(audio);
  return audio;
}

const uint8_t *speech_audio_data(SpeechAudio *audio, size_t &bytes, bool &done) {
  done = audio->done.load(); // Before bytes, so a finished stream reports its final length.
  bytes = audio->bytes.load();
  return reinterpret_cast<const uint8_t *>(audio->pcm);
}

void release_speech_audio(SpeechAudio *audio) {
  if (audio) release(audio);
}
#else
bool audio_reply_available() { return false; }
void request_speech_test(const char *) { Serial.println("Speech: audio is unavailable."); }
bool begin_audio_reply(const uint8_t *) { return false; }
void audio_reply_progress(size_t, bool) {}
SpeechAudio *take_speech_audio() { return nullptr; }
const uint8_t *speech_audio_data(SpeechAudio *, size_t &bytes, bool &done) {
  bytes = 0;
  done = true;
  return nullptr;
}
void release_speech_audio(SpeechAudio *) {}
#endif
