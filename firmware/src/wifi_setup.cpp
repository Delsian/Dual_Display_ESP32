#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <atomic>
#include "config.h"
#include "drawing_tools.h"
#include "wifi_setup.h"
#include "device_config.h"
#include "speech_test.h"

namespace {
std::atomic<bool> portal_active{false};
// Initialized once by the worker before publishing portal_active.
char ap_name[24];

void report_access_point() {
  wifi_config_t config = {};
  const esp_err_t result = esp_wifi_get_config(WIFI_IF_AP, &config);
  Serial.printf("Wi-Fi AP: mode=%d, config=%s, IP=%s\n",
                static_cast<int>(WiFi.getMode()), esp_err_to_name(result),
                WiFi.softAPIP().toString().c_str());
  if (result == ESP_OK) {
    Serial.printf("Wi-Fi AP: SSID=%.*s, channel=%u, hidden=%u, clients=%u\n",
                  static_cast<int>(config.ap.ssid_len),
                  reinterpret_cast<const char *>(config.ap.ssid),
                  config.ap.channel, config.ap.ssid_hidden, WiFi.softAPgetStationNum());
  }
}

void wifi_task(void *) {
  const DeviceConfig &config = device_config();
  WiFiManager manager;
  // Report AP startup failures without verbose credential logging.
  manager.setDebugOutput(true, WM_DEBUG_ERROR);
  manager.setConfigPortalBlocking(false);
  manager.setConfigPortalTimeout(config.wifi_setup_timeout_seconds);
  manager.setConnectTimeout(config.wifi_connect_timeout_seconds);
  manager.setSaveConnectTimeout(config.wifi_connect_timeout_seconds);
  manager.setAPClientCheck(false);
  manager.setWebPortalClientCheck(false);
  manager.setShowPassword(false);
  const char *menu[] = {"wifi", "exit"};
  manager.setMenu(menu, 2);
  manager.setTitle(PROJECT_NAME " Wi-Fi setup");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(ap_name, sizeof(ap_name), PROJECT_NAME "-%06lX", (unsigned long)(mac & 0xffffff));

  auto start_portal = [&]() {
    if (manager.getConfigPortalActive()) {
      report_access_point();
      return;
    }
    manager.startConfigPortal(ap_name);
    report_access_point();
    portal_active.store(manager.getConfigPortalActive());
    if (portal_active.load()) {
      Serial.printf("Wi-Fi setup: join %s (no password), open http://192.168.4.1\n",
                    ap_name);
    } else {
      Serial.println("Wi-Fi setup could not start; send wifi to retry.");
    }
  };

  String saved_ssid = manager.getWiFiSSID();
  String saved_password = manager.getWiFiPass();
  unsigned next_network = 0;
  unsigned long attempt_time = 0;
  auto try_next_network = [&]() -> bool {
    const unsigned count = config.wifi_network_count + (saved_ssid.length() ? 1 : 0);
    if (next_network >= count) return false;
    const bool from_json = next_network < config.wifi_network_count;
    const char *ssid = from_json ? config.wifi_networks[next_network].ssid.c_str() : saved_ssid.c_str();
    const char *password = from_json ? config.wifi_networks[next_network].password.c_str() : saved_password.c_str();
    ++next_network;
    WiFi.persistent(false); // Trying JSON entries must not overwrite portal credentials in NVS.
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    attempt_time = millis();
    Serial.printf("Wi-Fi: trying network %u of %u.\n", next_network, count);
    return true;
  };
  bool startup_pending = true;
  bool connection_pending = try_next_network();
  if (!connection_pending) {
    startup_pending = false;
    start_portal();
  }
  unsigned long last_retry = millis();
  bool was_connected = false;
  char command[16] = {};
  size_t command_length = 0;
  bool command_overflow = false;
  Serial.println("Send wifi followed by Enter to change Wi-Fi.");
  Serial.println("Send speech followed by Enter to test the AI voice output.");

  for (;;) {
    // Bound serial work so a continuous input stream cannot starve networking.
    for (int n = 0; n < 32 && Serial.available(); ++n) {
      const char c = Serial.read();
      if (c == '\r' || c == '\n') {
        command[command_length] = '\0';
        if (!command_overflow && strcmp(command, "wifi") == 0) {
          startup_pending = false;
          connection_pending = false;
          start_portal();
        } else if (!command_overflow && strcmp(command, "speech") == 0) {
          request_speech_test();
        }
        command_length = 0;
        command_overflow = false;
      } else if (command_length < sizeof(command) - 1) {
        command[command_length++] = c;
      } else {
        command_overflow = true;
      }
    }

    manager.process();
    const bool active = manager.getConfigPortalActive();
    if (portal_active.exchange(active) && !active) {
      Serial.println("Wi-Fi setup closed. Send wifi to reopen.");
      last_retry = millis();
      saved_ssid = manager.getWiFiSSID();
      saved_password = manager.getWiFiPass();
    }
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != was_connected) {
      if (connected) Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
      else Serial.println("Wi-Fi disconnected; reconnecting in background.");
      was_connected = connected;
    }
    if (connected) {
      startup_pending = false;
      connection_pending = false;
    }
    if (connection_pending && !manager.getConfigPortalActive() &&
        millis() - attempt_time >= config.wifi_connect_timeout_seconds * 1000UL) {
      connection_pending = try_next_network();
      if (!connection_pending) {
        last_retry = millis();
        if (startup_pending) start_portal();
        startup_pending = false;
      }
    }
    if (!manager.getConfigPortalActive() && !connection_pending && !connected &&
        millis() - last_retry >= config.wifi_retry_interval_ms) {
      last_retry = millis();
      next_network = 0;
      connection_pending = try_next_network();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
} // namespace

void init_wifi() {
  // WiFiManager scans and credential submission can wait internally. Keep all
  // networking on this task, and all framebuffer access on the animation task.
  if (xTaskCreate(wifi_task, "wifi_setup", 8192, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("Wi-Fi task allocation failed; continuing offline.");
  }
}

bool draw_wifi_setup() {
  if (!portal_active.load()) return false;
  drawString_fb("Wi-Fi setup", 65, 52, TFT_WHITE);
  drawString_fb(ap_name, 30, 78, TFT_WHITE);
  drawString_fb("No password", 55, 114, TFT_WHITE);
  drawString_fb("192.168.4.1", 55, 152, TFT_WHITE);
  drawString_fb("Select home Wi-Fi", 35, 178, TFT_WHITE);
  return true;
}
