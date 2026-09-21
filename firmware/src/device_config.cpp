#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"
#include "device_config.h"

namespace {
constexpr const char *CONFIG_PATH = "/config.json";
constexpr const char *TEMP_PATH = "/config.json.tmp";
const DeviceConfig defaults = {AUDIO_OUTPUT_VOLUME, WIFI_PORTAL_TIMEOUT_SECONDS,
                               WIFI_CONNECT_TIMEOUT_SECONDS, WIFI_RETRY_INTERVAL_MS};
DeviceConfig current = defaults;

bool valid(const DeviceConfig &c) {
  if (c.wifi_network_count > MAX_WIFI_NETWORKS) return false;
  for (unsigned i = 0; i < c.wifi_network_count; ++i) {
    const auto &network = c.wifi_networks[i];
    if (network.ssid.length() == 0 || network.ssid.length() > 32 ||
        network.ssid.length() != strlen(network.ssid.c_str()) ||
        network.password.length() != strlen(network.password.c_str())) return false;
    const unsigned length = network.password.length();
    if (length != 0 && (length < 8 || length > 64)) return false;
    if (length == 64) {
      for (unsigned j = 0; j < length; ++j) {
        if (!isxdigit(static_cast<unsigned char>(network.password[j]))) return false;
      }
    }
  }
  return c.audio_volume <= 100 &&
         c.wifi_setup_timeout_seconds >= 30 && c.wifi_setup_timeout_seconds <= 3600 &&
         c.wifi_connect_timeout_seconds >= 1 && c.wifi_connect_timeout_seconds <= 120 &&
         c.wifi_retry_interval_ms >= 1000 && c.wifi_retry_interval_ms <= 3600000;
}

bool read_field(JsonObjectConst root, const char *key, uint32_t &value) {
  if (!root.containsKey(key)) return true; // Missing settings keep their defaults.
  if (!root[key].is<uint32_t>()) return false;
  value = root[key].as<uint32_t>();
  return true;
}

bool read_networks(JsonObjectConst root, DeviceConfig &config) {
  if (!root.containsKey("wifi_networks")) return true;
  if (!root["wifi_networks"].is<JsonArrayConst>()) return false;
  JsonArrayConst networks = root["wifi_networks"];
  if (networks.size() > MAX_WIFI_NETWORKS) return false;
  config.wifi_network_count = 0;
  for (JsonVariantConst entry : networks) {
    if (!entry.is<JsonObjectConst>() || !entry["ssid"].is<const char *>() ||
        !entry["password"].is<const char *>()) return false;
    auto &network = config.wifi_networks[config.wifi_network_count++];
    network.ssid = entry["ssid"].as<String>();
    network.password = entry["password"].as<String>();
  }
  return true;
}
}

const DeviceConfig &device_config() { return current; }

bool patch_device_config(const String &json, DeviceConfig &config) {
  if (json.length() > 4096) return false;
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, json) || !doc.is<JsonObject>()) return false;
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.size() == 0) return false;
  for (JsonPairConst field : root) {
    const String key = field.key().c_str();
    if (key != "version" && key != "audio_volume" && key != "wifi_setup_timeout_seconds" &&
        key != "wifi_connect_timeout_seconds" && key != "wifi_retry_interval_ms" &&
        key != "wifi_networks") return false;
  }
  if (root.containsKey("version") &&
      (!root["version"].is<uint32_t>() || root["version"].as<uint32_t>() != 1)) return false;
  DeviceConfig candidate = config;
  if (!read_field(root, "audio_volume", candidate.audio_volume) ||
      !read_field(root, "wifi_setup_timeout_seconds", candidate.wifi_setup_timeout_seconds) ||
      !read_field(root, "wifi_connect_timeout_seconds", candidate.wifi_connect_timeout_seconds) ||
      !read_field(root, "wifi_retry_interval_ms", candidate.wifi_retry_interval_ms) ||
      !read_networks(root, candidate) || !valid(candidate)) return false;
  config = candidate;
  return true;
}

String device_config_json(const DeviceConfig &config) {
  if (!valid(config)) return String();
  DynamicJsonDocument doc(4096);
  doc["version"] = 1;
  doc["audio_volume"] = config.audio_volume;
  doc["wifi_setup_timeout_seconds"] = config.wifi_setup_timeout_seconds;
  doc["wifi_connect_timeout_seconds"] = config.wifi_connect_timeout_seconds;
  doc["wifi_retry_interval_ms"] = config.wifi_retry_interval_ms;
  JsonArray networks = doc.createNestedArray("wifi_networks");
  for (unsigned i = 0; i < config.wifi_network_count; ++i) {
    JsonObject network = networks.createNestedObject();
    network["ssid"] = config.wifi_networks[i].ssid;
    network["password"] = config.wifi_networks[i].password;
  }
  if (doc.overflowed()) return String();
  String json;
  if (serializeJsonPretty(doc, json) != measureJsonPretty(doc)) return String();
  return json;
}

bool save_device_config(const DeviceConfig &config) {
  const String json = device_config_json(config);
  if (json.isEmpty() || json.length() > 8192) return false;
  File file = LittleFS.open(TEMP_PATH, "w");
  if (!file) return false;
  const size_t expected = json.length();
  const size_t written = file.print(json);
  file.flush();
  const bool complete = written == expected && file.size() == expected;
  file.close();
  // LittleFS rename replaces the old file atomically; never remove it first.
  if (!complete || !LittleFS.rename(TEMP_PATH, CONFIG_PATH)) {
    LittleFS.remove(TEMP_PATH);
    return false;
  }
  return true;
}

void load_device_config() {
  current = defaults;
  if (!LittleFS.exists(CONFIG_PATH)) {
    Serial.println(save_device_config(defaults) ? "Config: created /config.json."
                                               : "Config: could not save defaults; using defaults.");
    return;
  }
  File file = LittleFS.open(CONFIG_PATH, "r");
  DynamicJsonDocument doc(4096);
  if (!file || file.size() > 8192 || deserializeJson(doc, file)) {
    Serial.println("Config: unreadable JSON; using defaults, file preserved.");
    return;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  DeviceConfig candidate = defaults;
  if (root.isNull() || !root["version"].is<uint32_t>() || root["version"].as<uint32_t>() != 1 ||
      !read_field(root, "audio_volume", candidate.audio_volume) ||
      !read_field(root, "wifi_setup_timeout_seconds", candidate.wifi_setup_timeout_seconds) ||
      !read_field(root, "wifi_connect_timeout_seconds", candidate.wifi_connect_timeout_seconds) ||
      !read_field(root, "wifi_retry_interval_ms", candidate.wifi_retry_interval_ms) ||
      !read_networks(root, candidate) ||
      !valid(candidate)) {
    Serial.println("Config: invalid version or settings; using defaults, file preserved.");
    return;
  }
  current = candidate;
  Serial.println("Config: loaded /config.json.");
}
