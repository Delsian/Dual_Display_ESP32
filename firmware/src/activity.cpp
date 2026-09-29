#include <Arduino.h>
#include <esp_timer.h>
#include "activity.h"
#include "ble_config.h"

namespace {
portMUX_TYPE activity_lock = portMUX_INITIALIZER_UNLOCKED;
int64_t active_until = 0;
bool forced_sleep = false;
}

void restart_active_window() {
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&activity_lock);
  active_until = now + 5LL * 60 * 1000000;
  portEXIT_CRITICAL(&activity_lock);
}

bool device_active() {
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&activity_lock);
  const bool within_window = now < active_until;
  const bool sleeping = forced_sleep;
  portEXIT_CRITICAL(&activity_lock);
  return !sleeping && (ble_connected() || within_window);
}

void set_device_sleeping(bool sleeping) {
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&activity_lock);
  forced_sleep = sleeping;
  active_until = sleeping ? 0 : now + 5LL * 60 * 1000000;
  portEXIT_CRITICAL(&activity_lock);
}
