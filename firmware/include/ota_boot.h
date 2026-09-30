#pragma once
void ota_boot_begin();
void ota_boot_failed(const char *reason);
void ota_boot_poll(bool healthy);
