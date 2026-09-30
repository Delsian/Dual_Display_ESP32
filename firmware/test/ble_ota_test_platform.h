#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <functional>
struct StopWorker {};
inline uint32_t &clock_ms() { static uint32_t n = 0; return n; }
inline uint32_t millis() { return clock_ms(); }
#define pdMS_TO_TICKS(n) (n)
constexpr int pdTRUE = 1, pdPASS = 1;
struct FakeQueue { size_t size; std::deque<std::vector<uint8_t>> items; };
using QueueHandle_t = FakeQueue*;
inline QueueHandle_t xQueueCreate(int, size_t size) { return new FakeQueue{size, {}}; }
inline int xQueueSend(QueueHandle_t q, const void *m, int) {
  auto *p = static_cast<const uint8_t *>(m); q->items.emplace_back(p, p + q->size); return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t q, void *m, int) {
  if (q->items.empty()) throw StopWorker{};
  memcpy(m, q->items.front().data(), q->size); q->items.pop_front(); return pdTRUE;
}
inline void xQueueReset(QueueHandle_t q) { q->items.clear(); }
inline void vQueueDelete(QueueHandle_t q) { delete q; }
inline void vTaskDelay(int n) { clock_ms() += n; }
inline void vTaskDelete(void*) {}
inline int xTaskCreate(void (*)(void*), const char*, int, void*, int, void*) { return pdPASS; }
constexpr int ESP_GATT_PERM_WRITE_ENC_MITM=1, ESP_GATT_PERM_READ_ENC_MITM=2;
class BLECharacteristic;
struct BLECharacteristicCallbacks { virtual void onWrite(BLECharacteristic*) {} };
struct BLE2902 {
  bool subscribed=true;
  bool getNotifications() { return subscribed; }
  void setNotifications(bool n) { subscribed=n; }
  void setAccessPermissions(int) {}
};
struct BLECharacteristic {
  enum { PROPERTY_WRITE=1, PROPERTY_READ=2, PROPERTY_NOTIFY=4 };
  std::string value;
  void setValue(uint8_t *p, size_t n) { value.assign(reinterpret_cast<char*>(p), n); }
  std::string getValue() { return value; }
  void notify() {}
  void setAccessPermissions(int) {}
  void setCallbacks(BLECharacteristicCallbacks*) {}
  void addDescriptor(BLE2902*) {}
};
struct BLEService {
  BLECharacteristic *createCharacteristic(const char*, int) { return nullptr; }
  void start() {}
};
struct BLEServer {
  int getConnId() { return 0; }
  int getPeerMTU(int) { return 185; }
  BLEService *createService(const char*) { return nullptr; }
};
struct Log {
  void println(const char*) {}
  template<class... T> void printf(const char*, T...) {}
};
static Log DeviceLog;
inline bool audio_ota_quiesced() { return true; }
inline bool speech_request_busy() { return false; }
inline void cancel_voice_requests() {}
using esp_ota_handle_t=unsigned;
using esp_err_t=int;
constexpr int ESP_OK=0;
struct esp_partition_t { uint32_t address, size; };
static esp_partition_t running_partition{0x10000, 6553600}, inactive_partition{0x650000, 6553600};
static int fail_begin=0, fail_write=0, fail_end=0, fail_boot=0;
static int abort_calls=0, boot_calls=0, hash_frees=0, write_calls=0;
inline const esp_partition_t *esp_ota_get_next_update_partition(void*) { return &inactive_partition; }
inline const esp_partition_t *esp_ota_get_running_partition() { return &running_partition; }
inline int esp_ota_begin(const esp_partition_t*, uint32_t, esp_ota_handle_t *h) { *h=1; return fail_begin; }
inline int esp_ota_abort(esp_ota_handle_t) { ++abort_calls; return 0; }
inline int esp_ota_write(esp_ota_handle_t, const void*, size_t) { ++write_calls; return fail_write; }
inline int esp_ota_end(esp_ota_handle_t) { return fail_end; }
struct esp_app_desc_t { char version[32]; char project_name[32]; };
static const char *candidate_version = "1.0.1";
inline int esp_ota_get_partition_description(const esp_partition_t*, esp_app_desc_t *desc) {
  strcpy(desc->version, candidate_version); strcpy(desc->project_name, "Parrot"); return 0;
}
inline int esp_ota_set_boot_partition(const esp_partition_t*) { ++boot_calls; return fail_boot; }
inline void esp_restart() { throw StopWorker{}; }
struct mbedtls_sha256_context {};
inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) { ++hash_frees; }
inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context*, int) { return 0; }
inline int mbedtls_sha256_update_ret(mbedtls_sha256_context*, const void*, size_t) { return 0; }
inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context*, uint8_t *p) { memset(p, 0, 32); return 0; }
#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1
using esp_ota_img_states_t = int;
constexpr int ESP_OTA_IMG_PENDING_VERIFY=1;
static int boot_state=1, confirm_calls=0, reject_calls=0, confirm_result=0;
inline int esp_ota_get_state_partition(const esp_partition_t*, int *state) { *state=boot_state; return 0; }
inline int esp_ota_mark_app_valid_cancel_rollback() { ++confirm_calls; return confirm_result; }
inline int esp_ota_mark_app_invalid_rollback_and_reboot() { ++reject_calls; return -1; }
