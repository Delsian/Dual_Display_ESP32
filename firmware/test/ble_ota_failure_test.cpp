// Exercise the actual OTA worker with deterministic fake BLE/flash/clock APIs.
#define BLE_OTA_HOST_TEST
#include "../src/ble_ota.cpp"
#include <cassert>

void pump() { try { worker(nullptr); } catch (const StopWorker&) {} }
void packet(bool data, std::initializer_list<uint8_t> bytes) {
  Message m={}; m.generation=generation.load(); m.epoch=transfer_epoch.load();
  m.mtu=185; m.data=data; m.size=bytes.size();
  std::copy(bytes.begin(), bytes.end(), m.bytes); xQueueSend(queue, &m, 0);
}
void reset() {
  if (queue) vQueueDelete(queue);
  queue=xQueueCreate(4,sizeof(Message));
  static BLECharacteristic characteristic; static BLE2902 cccd;
  status=&characteristic; subscription=&cccd; cccd.subscribed=true;
  state=Idle; active=false; authenticated=true; connected=true; generation=0;
  transfer_epoch=0; queue_error=0; abort_requested=false;
  handle_open=hash_open=false; next_offset=0; clock_ms()=0;
  fail_begin=fail_write=fail_end=fail_boot=0;
  candidate_version="1.0.1";
  abort_calls=boot_calls=hash_frees=write_calls=0;
}
void begin() {
  Message m={}; m.mtu=185; m.generation=generation.load(); m.epoch=transfer_epoch.load();
  m.bytes[0]=1; m.bytes[1]=1; m.size=37;
  xQueueSend(queue,&m,0); pump();
}
void failed(uint8_t error) {
  assert(state==Error && !active && !handle_open && !hash_open);
  assert(uint8_t(status->value[5])==error);
  assert(boot_calls==0);
}
int main() {
  reset(); begin(); assert(state==Receiving && active);
  queue_error=Resource; abort_requested=true; pump(); failed(Aborted);
  assert(abort_calls==1 && hash_frees==1 && queue->items.empty());
  begin(); assert(state==Receiving && next_offset==0); // Fresh retry.
  ble_ota_disconnected(); pump(); failed(Disconnected);

  reset(); begin(); clock_ms()=30000; pump(); failed(Timeout);
  reset(); clock_ms()=UINT32_MAX-100; begin(); clock_ms()+=30000; pump(); failed(Timeout);
  reset(); fail_begin=1; begin(); failed(Flash); assert(abort_calls==0);
  reset(); begin(); fail_write=1; packet(true,{0,0,0,0,0xe9}); pump(); failed(Flash);
  assert(next_offset==0 && abort_calls==1 && hash_frees==1);

  reset(); begin(); expected_hash[0]=1; packet(true,{0,0,0,0,0xe9}); pump();
  packet(false,{2}); pump(); failed(Hash); assert(abort_calls==1);
  reset(); begin(); packet(true,{0,0,0,0,0xe9}); pump(); fail_end=1;
  packet(false,{2}); pump(); failed(Image); assert(abort_calls==0);
  reset(); begin(); packet(true,{0,0,0,0,0xe9}); pump(); fail_boot=1;
  packet(false,{2}); pump();
  assert(state==Error && !active && !handle_open && !hash_open);
  assert(uint8_t(status->value[5])==Boot && boot_calls==1 && abort_calls==0);
  reset(); begin(); clock_ms()=29999; packet(true,{1,0,0,0,0xe9}); pump();
  assert(state==Receiving && last_progress==0); // Rejected data cannot renew timeout.
  clock_ms()=30000; pump(); failed(Timeout);

  reset(); begin(); Message stale={}; stale.epoch=transfer_epoch; stale.mtu=185;
  stale.bytes[0]=1; stale.bytes[1]=1;
  abort_requested=true; pump(); xQueueSend(queue,&stale,0); pump(); failed(Aborted);
  reset(); begin(); packet(true,{0,0,0,0,0xe9}); pump(); packet(false,{2}); pump();
  assert(state==Complete && boot_calls==1 && abort_calls==0);
  reset(); begin(); packet(true,{0,0,0,0,0xe9}); pump();
  candidate_version=FIRMWARE_VERSION; packet(false,{2}); pump(); failed(Version);
  for (const char *version : {"0.0.1", "invalid", "01.0.1"}) {
    reset(); begin(); packet(true,{0,0,0,0,0xe9}); pump();
    candidate_version=version; packet(false,{2}); pump(); failed(Version);
  }
  vQueueDelete(queue); queue=nullptr;
}
