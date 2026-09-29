#include "device_log.h"
#include <Arduino.h>
#include "config.h"
#include "speech_test.h"
#include "ble_config.h"
#include "activity.h"

#if USE_AUDIO
#include "speech_clip.h"
#include <BLE2902.h>
#include <BLEServer.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <memory>
#include <new>

struct SpeechAudio {
  explicit SpeechAudio(int16_t *data) : pcm(data) {}
  int16_t *pcm;
  unsigned long generation = 0;
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
  const unsigned long generation = ble_disconnect_generation();
  if (!device_active()) return false;
  constexpr size_t MAX_CLIP_BYTES = 128 * 1024; // Ample bound for ten seconds of 16 kHz ADPCM plus headers.
  if (!speech_clip_name_valid(name)) { DeviceLog.println("Speech: invalid clip name."); return false; }
  char path[32];
  snprintf(path, sizeof(path), "/clips/%s.wav", name);
  const uint32_t load_started = millis();
  File file = LittleFS.open(path, "r");
  const size_t bytes = file ? file.size() : 0;
  if (!bytes || bytes > MAX_CLIP_BYTES) {
    DeviceLog.printf("Speech: %s missing or too large; upload the filesystem image.\n", path);
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
    DeviceLog.printf("Speech: cannot load %s (%s).\n", path, frames ? "memory or decode" : "invalid clip");
    return false;
  }
  audio->bytes.store(frames * 4);
  audio->generation = generation;
  audio->done.store(true);
  ready.store(audio);
  DeviceLog.printf("Speech timing: clip=%s, frames=%u, load=%lu ms, total=%lu ms\n", name, unsigned(frames),
                static_cast<unsigned long>(millis() - load_started),
                static_cast<unsigned long>(millis() - started));
  hand_off(audio);
  release(audio);
  return true;
}

// Picks a random clip name from /clips; returns false if none exist.
bool random_clip(char *name, size_t size, bool fallback_only = false) {
  File dir = LittleFS.open("/clips");
  size_t count = 0;
  for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
    const String file = f.name();
    if (fallback_only && !file.startsWith("off_")) continue;
    // Reservoir sampling: one pass, uniform choice without storing names.
    if (!file.endsWith(".wav") || file.length() - 4 >= size || random(++count) != 0) continue;
    snprintf(name, size, "%.*s", int(file.length() - 4), file.c_str());
  }
  return count > 0;
}

// The audio task appends while the voice task transmits. Even on disconnect,
// busy stays set until final is seen so the recording buffer cannot be reused.
struct UploadJob {
  const uint8_t *stereo = nullptr;
  std::atomic<size_t> bytes{0};
  std::atomic<bool> final{false};
  uint32_t started = 0; // Recording start.
  uint32_t stopped = 0; // Recording end; written before final.
  uint32_t generation = 0; // Connection that accepted this recording.
  bool offline = false;
};
UploadJob upload;
bool upload_active = false; // Audio task only.

void wait_for_recording(UploadJob *job) {
  while (!job->final.load()) vTaskDelay(pdMS_TO_TICKS(10));
}

// BLE voice link (Doc/BLE.md): the phone app relays each recording to the
// Gemini and writes back the chosen clip. Protocol messages on TX:
//   0x01 start [rate u16]  0x02 audio [seq u8][predictor i16][index u8][nibbles]
//   0x03 end [samples u32] 0x04 cancel
// RX accepts replies plus "play:<name>" for manual playback while idle.
constexpr char VOICE_SERVICE_UUID[] = "6b520010-7c8e-4c30-9aa8-45e626d39b01";
constexpr char VOICE_TX_UUID[] = "6b520011-7c8e-4c30-9aa8-45e626d39b01";
constexpr char VOICE_RX_UUID[] = "6b520012-7c8e-4c30-9aa8-45e626d39b01";
constexpr size_t PACKET_SAMPLES = 320; // 20 ms; 165-byte notifications fit MTU 185.
constexpr size_t MIN_UPLOAD_SAMPLES = 4000; // Worker rejects under 0.25 s.
constexpr uint32_t REPLY_TIMEOUT_MS = 30000;
constexpr uint16_t UPLOAD_RATE = 16000;

struct VoiceReply {
  char text[64];
};
struct VoiceRequest {
  UploadJob *recording = nullptr;
  char clip[17] = {};
  uint32_t generation = 0;
};
BLECharacteristic *voice_tx = nullptr;
BLE2902 *voice_subscription = nullptr;
QueueHandle_t replies = nullptr;
QueueHandle_t requests = nullptr;
std::atomic<bool> link_connected{false};
std::atomic<uint32_t> link_generation{0}; // Increments on disconnect.

bool link_ready() {
  return link_connected.load() && voice_subscription && voice_subscription->getNotifications();
}

bool send_message(const uint8_t *data, size_t bytes) {
  if (!link_ready()) return false;
  voice_tx->setValue(const_cast<uint8_t *>(data), bytes);
  voice_tx->notify();
  return true;
}

class VoiceRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    const std::string value = characteristic->getValue();
    if (value == "sleep" || value == "wakeup") {
      set_device_sleeping(value == "sleep");
      if (value == "sleep") link_generation.fetch_add(1);
      DeviceLog.println(value == "sleep" ? "Device: sleeping (BLE stays connected)." : "Device: awake.");
      return;
    }
    if (value.compare(0, 5, "play:") == 0) {
      const std::string name = value.substr(5);
      if (name.find('\0') != std::string::npos || !speech_clip_name_valid(name.c_str())) {
        DeviceLog.println("Speech: invalid manual clip name.");
        return;
      }
      if (!requests || !link_ready()) return;
      if (busy.exchange(true)) {
        DeviceLog.println("Speech: request already pending; manual play rejected.");
        return;
      }
      VoiceRequest request;
      snprintf(request.clip, sizeof(request.clip), "%s", name.c_str());
      request.generation = link_generation.load();
      if (xQueueSend(requests, &request, 0) != pdTRUE) busy.store(false);
      return;
    }
    VoiceReply reply = {};
    snprintf(reply.text, sizeof(reply.text), "%.*s", int(value.size()), value.data());
    if (replies) xQueueOverwrite(replies, &reply);
  }
};
VoiceRxCallbacks voice_rx_callbacks;

// Streams one recording to the phone while it is captured, then waits for the
// clip choice. Returns true once clip audio reached the audio task.
bool reply_to_recording(UploadJob *job) {
  const uint32_t generation = job->generation;
  auto connected = [&] { return link_ready() && link_generation.load() == generation; };
  auto fail = [&](const char *message) {
    const uint8_t cancel = 0x04;
    if (connected()) send_message(&cancel, 1);
    wait_for_recording(job);
    DeviceLog.println(message);
    return false;
  };
  if (!connected()) return fail("AI: phone not connected; recording not sent.");
  xQueueReset(replies);
  const uint8_t start[] = {0x01, UPLOAD_RATE & 0xff, UPLOAD_RATE >> 8};
  if (!send_message(start, sizeof(start))) return fail("AI: phone disconnected.");

  SpeechAdpcmState state;
  uint8_t packet[5 + PACKET_SAMPLES / 2];
  uint8_t sequence = 0;
  size_t sent = 0; // Stereo bytes consumed.
  const auto *stereo = reinterpret_cast<const int16_t *>(job->stereo);
  for (;;) {
    const bool final = job->final.load(); // Before bytes, so the final length is seen.
    const size_t ready = (job->bytes.load() - sent) / 4;
    if (ready < PACKET_SAMPLES && !(final && ready)) {
      if (final) break;
      if (!connected()) return fail("AI: phone disconnected during upload.");
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const size_t count = min(ready, PACKET_SAMPLES);
    packet[0] = 0x02;
    packet[1] = sequence++;
    packet[2] = uint16_t(state.predictor) & 0xff;
    packet[3] = uint16_t(state.predictor) >> 8;
    packet[4] = state.index;
    speech_adpcm_encode(state, stereo + sent / 2 + AUDIO_AI_MIC_CHANNEL, 2, count, packet + 5);
    if (!connected() || !send_message(packet, 5 + (count + 1) / 2)) {
      return fail("AI: phone disconnected during upload.");
    }
    sent += count * 4;
  }
  const uint32_t samples = sent / 4;
  if (samples < MIN_UPLOAD_SAMPLES) return fail("AI: recording under 0.25 seconds; upload cancelled.");
  const uint8_t end[] = {0x03, uint8_t(samples), uint8_t(samples >> 8), uint8_t(samples >> 16), uint8_t(samples >> 24)};
  if (!connected() || !send_message(end, sizeof(end))) return fail("AI: phone disconnected.");
  const uint32_t upload_done = millis();
  DeviceLog.printf("AI timing: upload_tail=%lu ms, audio=%u ms\n",
                static_cast<unsigned long>(upload_done - job->stopped), unsigned(samples / 16));

  VoiceReply reply;
  bool received = false;
  while (!received && connected() && millis() - upload_done < REPLY_TIMEOUT_MS) {
    received = xQueueReceive(replies, &reply, pdMS_TO_TICKS(100)) == pdTRUE;
  }
  if (!received || !connected()) {
    DeviceLog.println(connected() ? "AI: no reply from phone." : "AI: phone disconnected before replying.");
    return false;
  }
  DeviceLog.printf("AI timing: reply=%lu ms after upload, total=%lu ms\n",
                static_cast<unsigned long>(millis() - upload_done),
                static_cast<unsigned long>(millis() - job->stopped));
  DeviceLog.printf("AI reply: %s\n", reply.text);
  if (strncmp(reply.text, "clip:", 5) == 0) return play_clip(reply.text + 5, job->stopped);
  return false; // "ignore" plays nothing; errors are printed above.
}

void voice_task(void *) {
  for (;;) {
    VoiceRequest request;
    if (xQueueReceive(requests, &request, portMAX_DELAY) != pdTRUE) continue;
    bool played = false;
    if (request.recording && request.recording->offline) {
      auto *job = request.recording;
      wait_for_recording(job);
      char name[20];
      if (device_active() && job->generation == link_generation.load() &&
          job->bytes.load() / 4 >= MIN_UPLOAD_SAMPLES) {
        if (random_clip(name, sizeof(name), true)) played = play_clip(name, job->started);
        else DeviceLog.println("Speech: no fallback clips in /clips.");
      }
    } else {
      played = request.recording ? reply_to_recording(request.recording) :
          (link_ready() && link_generation.load() == request.generation && play_clip(request.clip, millis()));
    }
    if (!played) busy.store(false);
  }
}
}

void init_voice_link(BLEServer *server) {
  replies = xQueueCreate(1, sizeof(VoiceReply));
  requests = xQueueCreate(1, sizeof(VoiceRequest));
  if (!replies || !requests ||
      xTaskCreate(voice_task, "voice_link", 4096, nullptr, 1, nullptr) != pdPASS) {
    if (replies) vQueueDelete(replies);
    if (requests) vQueueDelete(requests);
    replies = requests = nullptr;
    DeviceLog.println("Voice link: allocation failed; AI requests disabled.");
    return;
  }
  BLEService *service = server->createService(VOICE_SERVICE_UUID);
  voice_tx = service->createCharacteristic(VOICE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  voice_subscription = new BLE2902();
  // Subscribing requires the bonded, authenticated link, as config writes do.
  voice_subscription->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);
  voice_tx->addDescriptor(voice_subscription);
  auto *rx = service->createCharacteristic(VOICE_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  rx->setCallbacks(&voice_rx_callbacks);
  service->start();
}

void voice_link_connected() { link_connected.store(true); }

void voice_link_disconnected() {
  link_generation.fetch_add(1);
  link_connected.store(false);
  if (voice_subscription) voice_subscription->setNotifications(false);
}

// Serial "speech [N|off_K]": plays a clip from LittleFS without the network.
void request_speech_test(const char *clip) {
  char name[20];
  if (!clip || !*clip) {
    if (!random_clip(name, sizeof(name))) {
      DeviceLog.println("Speech: no clips in /clips; upload the filesystem image.");
      return;
    }
  } else {
    char *end = nullptr;
    const long id = strtol(clip, &end, 10);
    if (*end == '\0' && id >= 1 && id <= 999) snprintf(name, sizeof(name), "%03ld", id);
    else snprintf(name, sizeof(name), "%s", clip);
  }
  if (busy.exchange(true)) { DeviceLog.println("Speech: request already pending."); return; }
  DeviceLog.printf("Speech: local clip %s.\n", name);
  if (!play_clip(name, millis())) busy.store(false);
}

bool audio_reply_available() {
  return requests && device_active() && (link_ready() || !ble_connected()) && !busy.load();
}

bool begin_audio_reply(const uint8_t *stereo) {
  static_assert(AUDIO_AI_MIC_CHANNEL <= 1 && AUDIO_AI_MIC_CHANNEL >= 0, "Invalid microphone channel");
  if (!stereo || !audio_reply_available()) return false;
  const uint32_t generation = link_generation.load();
  if (busy.exchange(true)) { DeviceLog.println("AI: request already pending; recording not sent."); return false; }
  // Busy was clear, so the voice task no longer reads the previous job.
  upload.stereo = stereo;
  upload.bytes.store(0);
  upload.final.store(false);
  upload.started = millis();
  upload.generation = generation;
  upload.offline = !ble_connected();
  VoiceRequest request;
  request.recording = &upload;
  if (xQueueSend(requests, &request, 0) != pdTRUE) {
    busy.store(false);
    DeviceLog.println("AI: voice queue unavailable.");
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
  DeviceLog.printf("AI: recorded %u ms; finishing upload.\n", unsigned(bytes / 64));
}

SpeechAudio *take_speech_audio() {
  SpeechAudio *audio = ready.exchange(nullptr);
  if (audio) hand_off(audio);
  if (audio && (!device_active() || audio->generation != ble_disconnect_generation())) {
    release(audio);
    return nullptr;
  }
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
void init_voice_link(BLEServer *) {}
void voice_link_connected() {}
void voice_link_disconnected() {}
bool audio_reply_available() { return false; }
void request_speech_test(const char *) { DeviceLog.println("Speech: audio is unavailable."); }
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
