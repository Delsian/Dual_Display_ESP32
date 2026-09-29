#include "device_log.h"
#include "device_log_buffer.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <atomic>

namespace {
constexpr char SERVICE_UUID[] = "6b520020-7c8e-4c30-9aa8-45e626d39b01";
constexpr char TX_UUID[] = "6b520021-7c8e-4c30-9aa8-45e626d39b01";
DeviceLogBuffer buffer;
portMUX_TYPE buffer_lock = portMUX_INITIALIZER_UNLOCKED;
BLECharacteristic *log_tx = nullptr;
BLE2902 *subscription = nullptr;
std::atomic<uint32_t> generation{0};
bool worker_ready = false;

void reset_stream(bool enabled) {
  portENTER_CRITICAL(&buffer_lock);
  generation.fetch_add(1);
  buffer.reset(enabled);
  portEXIT_CRITICAL(&buffer_lock);
}

class SubscriptionCallbacks : public BLEDescriptorCallbacks {
  void onWrite(BLEDescriptor *) override {
    reset_stream(worker_ready && subscription->getNotifications());
  }
} subscription_callbacks;

void log_task(void *) {
  for (;;) {
    // At most 800 text bytes/s; logging never sends from audio/render tasks.
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t packet[DeviceLogBuffer::PACKET_SIZE];
    portENTER_CRITICAL(&buffer_lock);
    const uint32_t session = generation.load();
    const size_t size = buffer.take(packet);
    portEXIT_CRITICAL(&buffer_lock);
    if (!size || session != generation.load()) continue;
    log_tx->setValue(packet, size);
    if (session == generation.load()) log_tx->notify();
  }
}
} // namespace

DeviceLogPrint DeviceLog;

size_t DeviceLogPrint::write(uint8_t value) { return write(&value, 1); }

size_t DeviceLogPrint::write(const uint8_t *data, size_t size) {
  if (!size) return 0;
  portENTER_CRITICAL(&buffer_lock);
  buffer.append(data, size);
  portEXIT_CRITICAL(&buffer_lock);
  Serial.write(data, size);
  return size;
}

void init_device_log(BLEServer *server) {
  BLEService *service = server->createService(SERVICE_UUID);
  log_tx = service->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  subscription = new BLE2902();
  subscription->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);
  subscription->setCallbacks(&subscription_callbacks);
  log_tx->addDescriptor(subscription);
  worker_ready = xTaskCreate(log_task, "ble_logs", 3072, nullptr, 1, nullptr) == pdPASS;
  if (!worker_ready) Serial.println("BLE logs: task allocation failed; serial logs only.");
  service->start();
}

void device_log_disconnected() {
  reset_stream(false);
  if (subscription) {
    subscription->setNotifications(false);
    subscription->setIndications(false);
  }
}
