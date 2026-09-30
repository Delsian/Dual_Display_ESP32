#include "ble_ota.h"
#include "ble_ota_protocol.h"
#include "firmware_version.h"
#include "ota_version.h"
#ifdef BLE_OTA_HOST_TEST
#include "ble_ota_test_platform.h"
#else
#include "audio.h"
#include "speech_test.h"
#include "device_log.h"
#include <Arduino.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <atomic>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#endif

namespace {
using namespace BleOta;
constexpr uint32_t TIMEOUT_MS = 30000;
struct Message {
  uint32_t generation;
  uint32_t epoch;
  uint16_t mtu;
  bool data;
  uint8_t error;
  uint16_t size;
  uint8_t bytes[512]; // Maximum GATT characteristic value, bounded queue copy.
};
QueueHandle_t queue = nullptr;
bool worker_ready = false;
BLEServer *server = nullptr;
BLECharacteristic *status = nullptr;
BLE2902 *subscription = nullptr;
std::atomic<bool> authenticated{false}, connected{false}, active{false};
std::atomic<uint32_t> generation{0};
std::atomic<uint32_t> transfer_epoch{0};
std::atomic<uint8_t> queue_error{0};
std::atomic<bool> abort_requested{false};
// Session state belongs only to the worker.
State state = Idle;
esp_ota_handle_t handle = 0;
bool handle_open = false;
mbedtls_sha256_context hash;
bool hash_open = false;
uint8_t expected_hash[32];
uint32_t image_bytes = 0;
uint32_t next_offset = 0;
const esp_partition_t *target_partition = nullptr;
uint32_t session_generation = 0;
uint32_t last_progress = 0;

void publish(uint8_t error = None) {
  uint8_t packet[6] = {static_cast<uint8_t>(state), uint8_t(next_offset),
      uint8_t(next_offset >> 8), uint8_t(next_offset >> 16), uint8_t(next_offset >> 24), error};
  status->setValue(packet, sizeof(packet));
  if (connected.load() && authenticated.load() && subscription->getNotifications()) status->notify();
}

void cleanup(uint8_t error) {
  transfer_epoch.fetch_add(1); // Invalidate even callbacks racing queue reset.
  if (handle_open) esp_ota_abort(handle);
  handle_open = false;
  if (hash_open) mbedtls_sha256_free(&hash);
  hash_open = false;
  image_bytes = 0;
  target_partition = nullptr;
  abort_requested.store(false);
  queue_error.store(0); // Preserve the terminal failure rather than a stale rejection.
  xQueueReset(queue); // Drop commands queued for the session being released.
  state = Error;
  // Keep exclusivity through resource cleanup; no boot selection is changed.
  active.store(false);
  publish(error);
}

bool same_connection(uint32_t epoch) {
  return connected.load() && authenticated.load() && generation.load() == epoch;
}

class WriteCallbacks : public BLECharacteristicCallbacks {
 public:
  explicit WriteCallbacks(bool is_data) : data(is_data) {}
  void onWrite(BLECharacteristic *characteristic) override {
    if (!connected.load() || !authenticated.load()) return;
    const std::string value = characteristic->getValue();
    Message message = {};
    message.generation = generation.load();
    message.epoch = transfer_epoch.load();
    message.mtu = server->getPeerMTU(server->getConnId());
    message.data = data;
    if (data) {
      message.error = message.mtu < 3 || value.size() < 5 || value.size() > sizeof(message.bytes) || value.size() > size_t(message.mtu - 3)
          ? Malformed : None;
    } else {
      message.error = validate_control(reinterpret_cast<const uint8_t *>(value.data()), value.size());
    }
    if (!message.error) {
      message.size = value.size();
      memcpy(message.bytes, value.data(), value.size());
      // Abort must be noticed during Finish verification, not only afterward.
      if (!data && message.bytes[0] == 3 && active.load()) {
        abort_requested.store(true);
        return;
      }
    }
    if (xQueueSend(queue, &message, 0) != pdTRUE) queue_error.store(Resource);
  }
 private:
  bool data;
} control_callbacks(false), data_callbacks(true);

void worker(void *) {
  for (;;) {
    if (state == Complete) {
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_restart();
      continue;
    }
    if (active.load() && !same_connection(session_generation)) cleanup(Disconnected);
    if (active.load() && abort_requested.load()) cleanup(Aborted);
    if (active.load() && millis() - last_progress >= TIMEOUT_MS) cleanup(Timeout);
    const uint8_t rejected = queue_error.exchange(0);
    if (rejected) publish(rejected);
    Message message;
    if (xQueueReceive(queue, &message, pdMS_TO_TICKS(20)) != pdTRUE) continue;
    if (message.epoch != transfer_epoch.load() || !same_connection(message.generation)) continue;
    // Cancellation/timeout may arrive while waiting for the next packet.
    if (active.load() && abort_requested.load()) { cleanup(Aborted); continue; }
    if (active.load() && millis() - last_progress >= TIMEOUT_MS) { cleanup(Timeout); continue; }
    if (message.error) { publish(message.error); continue; }
    if (message.mtu < 40) { publish(Mtu); continue; }
    if (message.data) {
      if (state != Receiving) { publish(InvalidState); continue; }
      const uint8_t error = validate_data(message.bytes, message.size, message.mtu, next_offset, image_bytes);
      if (error) { publish(error); continue; }
      const size_t count = message.size - 4;
      if (esp_ota_write(handle, message.bytes + 4, count) != ESP_OK) { cleanup(Flash); continue; }
      if (mbedtls_sha256_update_ret(&hash, message.bytes + 4, count) != 0) { cleanup(Resource); continue; }
      next_offset += count;
      last_progress = millis();
      if (!same_connection(session_generation)) { cleanup(Disconnected); continue; }
      if (abort_requested.load()) { cleanup(Aborted); continue; }
      publish(); // Application ACK only after both flash and hash accept the bytes.
      continue;
    }
    if (message.bytes[0] == 2) {
      if (state != Receiving || next_offset != image_bytes) { publish(InvalidState); continue; }
      state = Verifying;
      publish();
      uint8_t digest[32];
      if (mbedtls_sha256_finish_ret(&hash, digest) != 0) { cleanup(Resource); continue; }
      if (memcmp(digest, expected_hash, sizeof(digest)) != 0) { cleanup(Hash); continue; }
      mbedtls_sha256_free(&hash);
      hash_open = false;
      const esp_err_t validation = esp_ota_end(handle);
      handle_open = false; // esp_ota_end consumes the handle even on failure.
      if (validation != ESP_OK) { cleanup(Image); continue; }
      esp_app_desc_t candidate = {};
      if (esp_ota_get_partition_description(target_partition, &candidate) != ESP_OK ||
          memcmp(candidate.project_name, "Parrot\0", 7) != 0 ||
          !ota_version_higher(candidate.version, sizeof(candidate.version),
                              FIRMWARE_VERSION, sizeof(FIRMWARE_VERSION))) {
        cleanup(Version);
        continue;
      }
      if (!same_connection(session_generation)) { cleanup(Disconnected); continue; }
      if (abort_requested.load()) { cleanup(Aborted); continue; }
      // Final activation is serialized in this worker. Once boot selection
      // starts, later Abort/disconnect cannot revoke the committed update.
      if (esp_ota_set_boot_partition(target_partition) != ESP_OK) { cleanup(Boot); continue; }
      state = Complete;
      publish();
      continue;
    }
    if (message.bytes[0] == 3) {
      if (active.load()) cleanup(Aborted);
      else publish();
      continue;
    }
    if (active.load()) { publish(InvalidState); continue; }
    if (!subscription->getNotifications()) { publish(InvalidState); continue; }
    const uint32_t size = image_size(message.bytes);
    const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!partition || !running || partition->address == running->address) {
      state = Error;
      publish(Partition);
      continue;
    }
    if (!size || size > partition->size) { publish(Bounds); continue; }
    session_generation = message.generation;
    image_bytes = size;
    next_offset = 0;
    target_partition = partition;
    abort_requested.store(false);
    memcpy(expected_hash, message.bytes + 5, sizeof(expected_hash));
    active.store(true);
    cancel_voice_requests();
    state = Preparing;
    last_progress = millis();
    publish();
    // Let audio finalize its shared capture buffer and mute before flash erase.
    while ((!audio_ota_quiesced() || speech_request_busy()) &&
           same_connection(session_generation) && !abort_requested.load() && millis() - last_progress < 2000) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!same_connection(session_generation)) { cleanup(Disconnected); continue; }
    if (abort_requested.load()) { cleanup(Aborted); continue; }
    if (!audio_ota_quiesced() || speech_request_busy()) { cleanup(Resource); continue; }
    if (esp_ota_begin(partition, image_bytes, &handle) != ESP_OK) { cleanup(Flash); continue; }
    handle_open = true;
    if (!same_connection(session_generation)) { cleanup(Disconnected); continue; }
    if (abort_requested.load()) { cleanup(Aborted); continue; }
    mbedtls_sha256_init(&hash);
    hash_open = true;
    if (mbedtls_sha256_starts_ret(&hash, 0) != 0) { cleanup(Resource); continue; }
    state = Receiving;
    last_progress = millis();
    publish();
  }
}
}

bool ble_ota_active() { return active.load(); }
bool ble_ota_ready() { return worker_ready; }
void ble_ota_connected() {
  authenticated.store(false);
  connected.store(true);
}
void ble_ota_authenticated(bool value) { authenticated.store(value); }
void ble_ota_disconnected() {
  connected.store(false);
  authenticated.store(false);
  generation.fetch_add(1);
  queue_error.store(0);
  if (subscription) subscription->setNotifications(false);
}

void init_ble_ota(BLEServer *gatt) {
  server = gatt;
  queue = xQueueCreate(4, sizeof(Message));
  if (!queue) { DeviceLog.println("OTA: queue allocation failed."); return; }
  auto *service = server->createService("6b520030-7c8e-4c30-9aa8-45e626d39b01");
  auto *control = service->createCharacteristic("6b520031-7c8e-4c30-9aa8-45e626d39b01", BLECharacteristic::PROPERTY_WRITE);
  auto *data = service->createCharacteristic("6b520032-7c8e-4c30-9aa8-45e626d39b01", BLECharacteristic::PROPERTY_WRITE);
  status = service->createCharacteristic("6b520033-7c8e-4c30-9aa8-45e626d39b01", BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  control->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  data->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  status->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  subscription = new BLE2902();
  subscription->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);
  status->addDescriptor(subscription);
  control->setCallbacks(&control_callbacks);
  data->setCallbacks(&data_callbacks);
  uint8_t initial[6] = {};
  status->setValue(initial, sizeof(initial));
  if (xTaskCreate(worker, "ble_ota", 6144, nullptr, 1, nullptr) != pdPASS) {
    vQueueDelete(queue);
    queue = nullptr;
    DeviceLog.println("OTA: worker allocation failed; service not started.");
    return;
  }
  service->start();
  worker_ready = true;
}
