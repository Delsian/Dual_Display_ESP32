#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

void init_ble_config(int battery_percentage);
void update_ble_battery(int battery_percentage);
bool draw_ble_pairing();

#endif
