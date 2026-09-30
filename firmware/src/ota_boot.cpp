#include "ota_boot.h"
#ifdef BLE_OTA_HOST_TEST
#include "ble_ota_test_platform.h"
#else
#include "device_log.h"
#include <Arduino.h>
#include <atomic>
#include <esp_ota_ops.h>
#include <sdkconfig.h>
#endif

#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Parrot OTA requires a rollback-enabled SDK and bootloader"
#endif

// Arduino otherwise marks a pending image valid before setup() runs.
extern "C" bool verifyRollbackLater() { return true; }

namespace {
std::atomic<bool> pending{false};
bool healthy_started = false;
uint32_t healthy_since = 0;

void reject(const char *reason) {
  if (!pending.exchange(false)) return;
  DeviceLog.printf("OTA startup rejected: %s; requesting rollback.\n", reason);
  const esp_err_t result = esp_ota_mark_app_invalid_rollback_and_reboot();
  // A missing/invalid previous image prevents automatic recovery. Never confirm
  // this image after rejection; leave recovery to USB rather than reboot-looping.
  DeviceLog.printf("OTA rollback failed (%d); USB recovery required.\n", int(result));
}

void startup_deadline(void *) {
  vTaskDelay(pdMS_TO_TICKS(60000));
  reject("startup deadline exceeded");
  vTaskDelete(nullptr);
}
}

void ota_boot_begin() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK ||
      state != ESP_OTA_IMG_PENDING_VERIFY) return;
  pending.store(true);
  DeviceLog.println("OTA: pending image; checking startup before confirmation.");
  if (xTaskCreate(startup_deadline, "ota_boot", 4096, nullptr, 2, nullptr) != pdPASS) {
    reject("cannot create startup watchdog");
  }
}

void ota_boot_failed(const char *reason) { reject(reason); }

void ota_boot_poll(bool healthy) {
  if (!pending.load()) return;
  if (!healthy) { reject("audio or OTA service unavailable"); return; }
  if (!healthy_started) { healthy_started = true; healthy_since = millis(); }
  if (millis() - healthy_since < 5000) return;
  if (!pending.exchange(false)) return;
  const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
  if (result == ESP_OK) {
    DeviceLog.println("OTA: startup healthy; new firmware confirmed.");
  } else {
    pending.store(true);
    reject("cannot confirm image");
  }
}
