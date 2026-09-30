#pragma once
class BLEServer;
void init_ble_ota(BLEServer *server);
void ble_ota_connected();
void ble_ota_authenticated(bool authenticated);
void ble_ota_disconnected();
bool ble_ota_active();
bool ble_ota_ready();
