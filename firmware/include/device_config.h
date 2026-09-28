#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdint.h>
#include <Arduino.h>

struct DeviceConfig {
  uint32_t audio_volume;
};

// Call once after mounting LittleFS, before starting audio/BLE tasks.
void load_device_config();
const DeviceConfig &device_config();
// Persist for the next boot; does not mutate the running configuration.
bool save_device_config(const DeviceConfig &config);
// Merge supported JSON fields into config only if the entire patch is valid.
bool patch_device_config(const String &json, DeviceConfig &config);
String device_config_json(const DeviceConfig &config);

#endif
