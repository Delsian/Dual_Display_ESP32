#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdint.h>
#include <Arduino.h>

constexpr unsigned MAX_WIFI_NETWORKS = 8;
struct WifiNetwork {
  String ssid;
  String password;
};

struct DeviceConfig {
  uint32_t audio_volume;
  uint32_t wifi_setup_timeout_seconds;
  uint32_t wifi_connect_timeout_seconds;
  uint32_t wifi_retry_interval_ms;
  unsigned wifi_network_count;
  WifiNetwork wifi_networks[MAX_WIFI_NETWORKS];
};

// Call once after mounting LittleFS, before starting audio/network tasks.
void load_device_config();
const DeviceConfig &device_config();
// Persist for the next boot; does not mutate the running configuration.
bool save_device_config(const DeviceConfig &config);
// Merge supported JSON fields into config only if the entire patch is valid.
bool patch_device_config(const String &json, DeviceConfig &config);
String device_config_json(const DeviceConfig &config);

#endif
