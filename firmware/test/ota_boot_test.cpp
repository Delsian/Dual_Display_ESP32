#define BLE_OTA_HOST_TEST
#include "../src/ota_boot.cpp"
#include <cassert>

void reset() {
  pending=false; healthy_started=false; healthy_since=0; clock_ms()=0;
  boot_state=1; confirm_calls=reject_calls=confirm_result=0;
}
int main() {
  assert(verifyRollbackLater());
  reset(); boot_state=0; ota_boot_begin(); ota_boot_poll(true);
  assert(!pending && confirm_calls==0 && reject_calls==0);
  reset(); ota_boot_begin(); ota_boot_poll(true); clock_ms()=4999; ota_boot_poll(true);
  assert(confirm_calls==0); clock_ms()=5000; ota_boot_poll(true);
  assert(confirm_calls==1 && !pending); startup_deadline(nullptr); assert(reject_calls==0);
  reset(); ota_boot_begin(); ota_boot_failed("mount"); ota_boot_poll(true);
  assert(reject_calls==1 && confirm_calls==0 && !pending);
  reset(); ota_boot_begin(); ota_boot_poll(false); assert(reject_calls==1);
  reset(); ota_boot_begin(); startup_deadline(nullptr); ota_boot_poll(true);
  assert(reject_calls==1 && confirm_calls==0);
  reset(); ota_boot_begin(); ota_boot_poll(true); confirm_result=-1;
  clock_ms()=5000; ota_boot_poll(true); assert(confirm_calls==1 && reject_calls==1 && !pending);
}
