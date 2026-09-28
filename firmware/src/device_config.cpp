#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"
#include "device_config.h"

namespace {
constexpr const char *CONFIG_PATH = "/config.json";
constexpr const char *TEMP_PATH = "/config.json.tmp";
const DeviceConfig defaults = {AUDIO_OUTPUT_VOLUME};
DeviceConfig current = defaults;

bool valid(const DeviceConfig &c) {
  return c.audio_volume <= 100;
}

bool read_field(JsonObjectConst root, const char *key, uint32_t &value) {
  if (!root.containsKey(key)) return true; // Missing settings keep their defaults.
  if (!root[key].is<uint32_t>()) return false;
  value = root[key].as<uint32_t>();
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
    if (key != "version" && key != "audio_volume") return false;
  }
  if (root.containsKey("version") &&
      (!root["version"].is<uint32_t>() || root["version"].as<uint32_t>() != 1)) return false;
  DeviceConfig candidate = config;
  if (!read_field(root, "audio_volume", candidate.audio_volume) || !valid(candidate)) return false;
  config = candidate;
  return true;
}

String device_config_json(const DeviceConfig &config) {
  if (!valid(config)) return String();
  DynamicJsonDocument doc(4096);
  doc["version"] = 1;
  doc["audio_volume"] = config.audio_volume;
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
      !valid(candidate)) {
    Serial.println("Config: invalid version or settings; using defaults, file preserved.");
    return;
  }
  current = candidate;
  Serial.println("Config: loaded /config.json.");
}
