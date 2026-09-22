#include <Arduino.h>
#include "config.h"
#include "speech_test.h"

#if USE_AUDIO && __has_include("speech_secrets.h")
#include "speech_secrets.h"
#include "speech_ca.h"
#include "speech_wav.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <memory>
#include <new>
#include <time.h>

namespace {
constexpr size_t MAX_WAV_BYTES = 480044;
std::atomic<bool> busy{false};
std::atomic<uint8_t *> ready{nullptr};
size_t ready_bytes = 0;

class PersistentHttp : public HTTPClient {
 public:
  bool establish() { return connect(); }
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
  explicit WavSink(uint8_t *data, size_t limit = MAX_WAV_BYTES) : buffer(data), capacity(limit) {}
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

uint8_t *download_speech(HttpSession &session, size_t &bytes, const String &text = String()) {
  if (!network_ready()) return nullptr;
  auto &http = session.http;
  // writeToStream decodes HTTP chunked transfer into the bounded WAV sink.
  if (!session.begin(text.isEmpty() ? "https://parrot.eug-krashtan.workers.dev/test-speech" :
                                    "https://parrot.eug-krashtan.workers.dev/speak")) return nullptr;
  http.addHeader("Authorization", String("Bearer ") + SPEECH_DEVICE_TOKEN);
  http.addHeader("Accept", "audio/wav");
  const char *headers[] = {"Content-Type"};
  http.collectHeaders(headers, 1);
  String payload;
  if (!text.isEmpty()) {
    DynamicJsonDocument body(8192);
    body["text"] = text;
    if (body.overflowed() || serializeJson(body, payload) == 0) {
      session.finish(false);
      return nullptr;
    }
    http.addHeader("Content-Type", "application/json");
  }
  const int status = http.POST(payload);
  Serial.printf("Speech: HTTP %d\n", status);
  if (status != 200 || http.header("Content-Type") != "audio/wav" ||
      http.getSize() > static_cast<int>(MAX_WAV_BYTES)) {
    session.finish(false);
    return nullptr;
  }
  using Buffer = std::unique_ptr<uint8_t, decltype(&heap_caps_free)>;
  Buffer wav(static_cast<uint8_t *>(heap_caps_malloc(MAX_WAV_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
  if (!wav) { session.finish(false); return nullptr; }
  WavSink sink(wav.get());
  const int received = http.writeToStream(&sink);
  session.finish(received >= 0 && static_cast<size_t>(received) == sink.used);
  if (received < 0 || static_cast<size_t>(received) != sink.used) return nullptr;
  const size_t frames = speech_wav_frames(wav.get(), sink.used);
  if (!frames) { Serial.println("Speech: invalid WAV response."); return nullptr; }
  Buffer pcm(static_cast<uint8_t *>(heap_caps_malloc(frames * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
  if (!pcm || !speech_wav_convert(wav.get(), sink.used, reinterpret_cast<int16_t *>(pcm.get()), frames)) return nullptr;
  bytes = frames * 4;
  return pcm.release();
}

struct UploadJob {
  uint8_t *wav;
  size_t bytes;
  uint32_t started;
  uint32_t prepared;
};

void upload_reply(HttpSession &session, UploadJob *job, String &answer) {
  const uint32_t task_started = millis();
  if (!network_ready()) return;
  const uint32_t network_ready_at = millis();
  Serial.printf("AI timing: prepare=%lu ms, task_wait=%lu ms, network_ready=%lu ms, wav=%u bytes\n",
                static_cast<unsigned long>(job->prepared - job->started),
                static_cast<unsigned long>(task_started - job->prepared),
                static_cast<unsigned long>(network_ready_at - task_started), unsigned(job->bytes));
  auto &http = session.http;
  if (!session.begin("https://parrot.eug-krashtan.workers.dev/ask")) return;
  http.addHeader("Authorization", String("Bearer ") + SPEECH_DEVICE_TOKEN);
  http.addHeader("Content-Type", "audio/wav");
  const char *headers[] = {"Content-Type"};
  http.collectHeaders(headers, 1);
  const uint32_t request_started = millis();
  const int status = http.POST(job->wav, job->bytes);
  const uint32_t headers_at = millis();
  // Connection is measured above; POST sends the WAV and waits for headers.
  Serial.printf("AI timing: post_to_headers=%lu ms\n",
                static_cast<unsigned long>(headers_at - request_started));
  Serial.printf("AI: HTTP %d\n", status);
  constexpr size_t MAX_REPLY_BYTES = 8192;
  if (status != 200 || !http.header("Content-Type").startsWith("application/json") ||
      http.getSize() > static_cast<int>(MAX_REPLY_BYTES)) {
    session.finish(false);
    Serial.println("AI: request failed; check Worker deployment, token, Wi-Fi, or quota.");
    return;
  }
  std::unique_ptr<uint8_t[]> reply(new (std::nothrow) uint8_t[MAX_REPLY_BYTES]);
  if (!reply) { session.finish(false); Serial.println("AI: response allocation failed."); return; }
  WavSink sink(reply.get(), MAX_REPLY_BYTES);
  const int received = http.writeToStream(&sink);
  const uint32_t body_at = millis();
  session.finish(received > 0 && static_cast<size_t>(received) == sink.used);
  if (received <= 0 || static_cast<size_t>(received) != sink.used) {
    Serial.println("AI: incomplete response.");
    return;
  }
  DynamicJsonDocument result(12288);
  const auto error = deserializeJson(result, reply.get(), sink.used);
  if (error || result["ok"] != true || !result["text"].is<const char *>()) {
    Serial.println("AI: invalid response.");
    return;
  }
  const uint32_t parsed_at = millis();
  Serial.printf("AI timing: response_body=%lu ms, parse=%lu ms, total=%lu ms\n",
                static_cast<unsigned long>(body_at - headers_at),
                static_cast<unsigned long>(parsed_at - body_at),
                static_cast<unsigned long>(parsed_at - job->started));
  const JsonObjectConst timing = result["timing_ms"].as<JsonObjectConst>();
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
  answer = result["text"].as<const char *>();
}

QueueHandle_t requests = nullptr;

void network_task(void *) {
  HttpSession session;
  for (;;) {
    UploadJob *job = nullptr; // Null means the fixed-phrase playback test.
    if (xQueueReceive(requests, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
      if (WiFi.status() != WL_CONNECTED || millis() - session.last_used >= 60000UL) {
        session.finish(false);
      }
      continue;
    }
    // Keep modem sleep enabled: this firmware runs Wi-Fi and BLE together.
    if (WiFi.status() != WL_CONNECTED) session.finish(false);
    if (job) {
      String answer;
      upload_reply(session, job, answer);
      heap_caps_free(job->wav);
      delete job;
      size_t bytes = 0;
      uint8_t *pcm = nullptr;
      if (!answer.isEmpty()) {
        Serial.println("Speech: requesting AI reply voice.");
        pcm = download_speech(session, bytes, answer);
      }
      if (pcm) {
        ready_bytes = bytes;
        ready.store(pcm);
        Serial.println("Speech: ready for playback when audio is idle.");
      } else {
        if (!answer.isEmpty()) Serial.println("Speech: reply audio failed; text remains available above.");
        busy.store(false);
      }
    } else {
      size_t bytes = 0;
      uint8_t *pcm = download_speech(session, bytes);
      if (pcm) {
        ready_bytes = bytes;
        ready.store(pcm);
        Serial.println("Speech: ready for playback when audio is idle.");
      } else {
        Serial.println("Speech: download failed; send speech to retry.");
        busy.store(false);
      }
    }
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
}

void request_speech_test() {
  if (busy.exchange(true)) { Serial.println("Speech: request already pending."); return; }
  if (!enqueue(nullptr)) {
    busy.store(false);
    Serial.println("Speech: task allocation failed.");
  } else {
    Serial.println("Speech: requesting fixed phrase.");
  }
}

bool audio_reply_available() {
  return WiFi.status() == WL_CONNECTED && !busy.load();
}

bool request_audio_reply(const uint8_t *stereo, size_t bytes) {
  const uint32_t started = millis();
  static_assert(AUDIO_AI_MIC_CHANNEL <= 1 && AUDIO_AI_MIC_CHANNEL >= 0, "Invalid microphone channel");
  if (bytes < 16000 || bytes > 1920000 || bytes % 4) {
    Serial.println("AI: record between 0.25 and 30 seconds.");
    return false;
  }
  if (busy.exchange(true)) { Serial.println("AI: request already pending; recording not sent."); return false; }
  const size_t wav_bytes = 44 + bytes / 2;
  auto *wav = static_cast<uint8_t *>(heap_caps_malloc(wav_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto *job = new (std::nothrow) UploadJob{wav, wav_bytes, started, 0};
  if (!job || !recording_wav(stereo, bytes, AUDIO_AI_MIC_CHANNEL, wav, wav_bytes)) {
    heap_caps_free(wav);
    delete job;
    busy.store(false);
    Serial.println("AI: recording allocation or conversion failed.");
    return false;
  }
  job->prepared = millis();
  if (!enqueue(job)) {
    heap_caps_free(wav);
    delete job;
    busy.store(false);
    Serial.println("AI: task allocation failed.");
    return false;
  }
  Serial.printf("AI: uploading %u ms of microphone audio.\n", unsigned(bytes / 64));
  return true;
}

uint8_t *take_speech_test(size_t &bytes) {
  uint8_t *pcm = ready.exchange(nullptr);
  if (pcm) { bytes = ready_bytes; busy.store(false); }
  return pcm;
}
#else
bool audio_reply_available() { return false; }
void request_speech_test() { Serial.println("Speech: audio or local device token is unavailable."); }
bool request_audio_reply(const uint8_t *, size_t) {
  Serial.println("AI: audio or local device token is unavailable.");
  return false;
}
uint8_t *take_speech_test(size_t &) { return nullptr; }
#endif
