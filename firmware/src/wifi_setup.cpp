#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <atomic>
#include "config.h"
#include "drawing_tools.h"
#include "wifi_setup.h"

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
  WiFiManager manager;
  // Report AP startup failures without verbose credential logging.
  manager.setDebugOutput(true, WM_DEBUG_ERROR);
  manager.setConfigPortalBlocking(false);
  manager.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_SECONDS);
  manager.setConnectTimeout(WIFI_CONNECT_TIMEOUT_SECONDS);
  manager.setSaveConnectTimeout(WIFI_CONNECT_TIMEOUT_SECONDS);
  manager.setAPClientCheck(false);
  manager.setWebPortalClientCheck(false);
  manager.setShowPassword(false);
  const char *menu[] = {"wifi", "exit"};
  manager.setMenu(menu, 2);
  manager.setTitle("DualEye Wi-Fi setup");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(ap_name, sizeof(ap_name), "DualEye-%06lX", (unsigned long)(mac & 0xffffff));

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

  bool startup_pending = manager.getWiFiIsSaved();
  if (startup_pending) WiFi.begin(); // Load credentials from ESP32 NVS.
  else start_portal();
  const unsigned long startup_time = millis();
  unsigned long last_retry = millis();
  bool was_connected = false;
  char command[16] = {};
  size_t command_length = 0;
  bool command_overflow = false;
  Serial.println("Send wifi followed by Enter to change Wi-Fi.");

  for (;;) {
    // Bound serial work so a continuous input stream cannot starve networking.
    for (int n = 0; n < 32 && Serial.available(); ++n) {
      const char c = Serial.read();
      if (c == '\r' || c == '\n') {
        command[command_length] = '\0';
        if (!command_overflow && strcmp(command, "wifi") == 0) {
          startup_pending = false;
          start_portal();
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
    }
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != was_connected) {
      if (connected) Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
      else Serial.println("Wi-Fi disconnected; reconnecting in background.");
      was_connected = connected;
    }
    if (startup_pending) {
      if (connected) startup_pending = false;
      else if (millis() - startup_time >= WIFI_CONNECT_TIMEOUT_SECONDS * 1000UL) {
        startup_pending = false;
        start_portal();
      }
    }
    if (!manager.getConfigPortalActive() && !startup_pending && !connected &&
        millis() - last_retry >= WIFI_RETRY_INTERVAL_MS) {
      last_retry = millis();
      if (manager.getWiFiIsSaved()) WiFi.reconnect();
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
