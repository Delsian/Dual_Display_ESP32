#pragma once

#include <Arduino.h>

class BLEServer;

// Application output goes to serial and, when subscribed, the BLE log stream.
// Do not use for credentials/passkeys or from interrupt handlers.
class DeviceLogPrint : public Print {
public:
  using Print::write;
  size_t write(uint8_t value) override;
  size_t write(const uint8_t *data, size_t size) override;
};

extern DeviceLogPrint DeviceLog;
void init_device_log(BLEServer *server);
void device_log_disconnected();
