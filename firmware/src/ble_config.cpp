#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <atomic>
#include "ble_config.h"
#include "device_config.h"
#include "drawing_tools.h"

namespace {
constexpr char SERVICE_UUID[] = "6b520001-7c8e-4c30-9aa8-45e626d39b01";
constexpr char PATCH_UUID[]   = "6b520002-7c8e-4c30-9aa8-45e626d39b01";
constexpr char CONTROL_UUID[] = "6b520003-7c8e-4c30-9aa8-45e626d39b01";
constexpr char RESULT_UUID[]  = "6b520004-7c8e-4c30-9aa8-45e626d39b01";
constexpr char DATA_UUID[]    = "6b520005-7c8e-4c30-9aa8-45e626d39b01";
constexpr size_t MAX_PATCH = 4096;
constexpr size_t PAGE_SIZE = 400;
std::atomic<uint32_t> pairing_code{0};
std::atomic<bool> pairing_visible{false};
DeviceConfig saved_config;
String patch;
bool overflow = false;
BLECharacteristic *result;
BLECharacteristic *data;

void status(const char *message) { result->setValue(message); }

class SecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return pairing_code.load(); }
  void onPassKeyNotify(uint32_t code) override {
    pairing_code.store(code);
    pairing_visible.store(true);
    Serial.printf("BLE pairing code: %06lu\n", static_cast<unsigned long>(code));
  }
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t) override { return false; } // Display-only passkey entry.
  void onAuthenticationComplete(esp_ble_auth_cmpl_t auth) override {
    pairing_visible.store(false);
    Serial.println(auth.success ? "BLE authenticated." : "BLE authentication failed.");
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onDisconnect(BLEServer *) override {
    patch = "";
    overflow = false;
    data->setValue("");
    status("ready");
    pairing_visible.store(false);
    BLEDevice::startAdvertising();
  }
};

class PatchCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    const std::string bytes = characteristic->getValue();
    if (overflow || bytes.find('\0') != std::string::npos ||
        patch.length() + bytes.size() > MAX_PATCH) {
      overflow = true;
      patch = "";
      status("error: clear first");
    } else if (!patch.concat(bytes.data(), bytes.size())) {
      overflow = true;
      patch = "";
      status("error: memory");
    } else {
      status("buffered");
    }
    characteristic->setValue("");
  }
};

class ControlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    const std::string command = characteristic->getValue();
    if (command == "clear") {
      patch = "";
      overflow = false;
      status("ready");
    } else if (command == "save") {
      DeviceConfig candidate = saved_config;
      if (overflow || !patch_device_config(patch, candidate)) {
        status("error: invalid JSON");
      } else if (!save_device_config(candidate)) {
        status("error: save failed");
      } else {
        saved_config = candidate;
        patch = "";
        status("saved; reboot needed");
      }
    } else if (command.compare(0, 5, "read:") == 0) {
      const std::string offset_text = command.substr(5);
      if (offset_text.empty() || offset_text.size() > 4 ||
          offset_text.find_first_not_of("0123456789") != std::string::npos) {
        status("error: offset");
        return;
      }
      const unsigned offset = strtoul(offset_text.c_str(), nullptr, 10);
      const String json = device_config_json(saved_config);
      if (json.isEmpty()) { status("error: memory"); return; }
      if (offset > json.length()) { status("error: offset"); return; }
      const String page = json.substring(offset, offset + PAGE_SIZE);
      data->setValue(std::string(page.c_str(), page.length()));
      status(offset + page.length() >= json.length() ? "end" : "more");
    } else {
      status("error: command");
    }
  }
};
SecurityCallbacks security_callbacks;
ServerCallbacks server_callbacks;
PatchCallbacks patch_callbacks;
ControlCallbacks control_callbacks;
} // namespace

void init_ble_config() {
  saved_config = device_config();
  char name[24];
  snprintf(name, sizeof(name), PROJECT_NAME "-%06lX", (unsigned long)(ESP.getEfuseMac() & 0xffffff));
  BLEDevice::init(name);
  BLEDevice::setMTU(185);
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  BLEDevice::setSecurityCallbacks(&security_callbacks);
  BLESecurity security;
  security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM);
  security.setCapability(ESP_IO_CAP_OUT);
  security.setKeySize(16);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(&server_callbacks);
  BLEService *service = server->createService(SERVICE_UUID);
  auto *input = service->createCharacteristic(PATCH_UUID, BLECharacteristic::PROPERTY_WRITE);
  input->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  input->setCallbacks(&patch_callbacks);
  auto *control = service->createCharacteristic(CONTROL_UUID, BLECharacteristic::PROPERTY_WRITE);
  control->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  control->setCallbacks(&control_callbacks);
  result = service->createCharacteristic(RESULT_UUID, BLECharacteristic::PROPERTY_READ);
  result->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  data = service->createCharacteristic(DATA_UUID, BLECharacteristic::PROPERTY_READ);
  data->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  status("ready");
  service->start();
  auto *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("BLE config ready: %s. Pair using the code shown on the left TFT.\n", name);
}

bool draw_ble_pairing() {
  if (!pairing_visible.load()) return false;
  char code[8];
  snprintf(code, sizeof(code), "%06lu", static_cast<unsigned long>(pairing_code.load()));
  drawString_fb("BLE pairing", 60, 80, TFT_WHITE);
  drawString_fb(code, 85, 110, TFT_WHITE);
  drawString_fb("Enter on phone", 40, 145, TFT_WHITE);
  return true;
}
