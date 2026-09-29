#include "device_log_buffer.h"
#include <assert.h>
#include <string>
#include <vector>

static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t(p[1]) << 8); }

int main() {
  DeviceLogBuffer buffer;
  uint8_t packet[DeviceLogBuffer::PACKET_SIZE] = {};
  const uint8_t text[] = "Audio: recording.\r\n";
  buffer.append(text, sizeof(text) - 1);
  assert(buffer.take(packet) == 0); // No replay of pre-subscription logs.
  buffer.reset(true);
  buffer.append(text, sizeof(text) - 1);
  std::string received;
  uint16_t sequence = 0;
  for (size_t n; (n = buffer.take(packet));) {
    assert(n > 4 && n <= 20);
    assert(u16(packet) == sequence++ && u16(packet + 2) == 0);
    received.append(reinterpret_cast<char *>(packet + 4), n - 4);
  }
  assert(received == reinterpret_cast<const char *>(text));

  // Repeated enqueue/dequeue crosses the ring boundary and sequence rollover.
  buffer.reset(true);
  for (unsigned i = 0; i < 65538; ++i) {
    uint8_t data[16];
    for (unsigned j = 0; j < 16; ++j) data[j] = (i + j) & 0xff;
    buffer.append(data, sizeof(data));
    assert(buffer.take(packet) == 20);
    assert(u16(packet) == uint16_t(i));
    assert(u16(packet + 2) == 0);
    assert(memcmp(packet + 4, data, 16) == 0);
  }

  buffer.reset(true);
  std::vector<uint8_t> flood(DeviceLogBuffer::CAPACITY + 31, 'x');
  buffer.append(flood.data(), flood.size());
  assert(buffer.take(packet) == 20 && u16(packet + 2) == 31);
  size_t retained = 16;
  for (size_t n; (n = buffer.take(packet));) {
    assert(u16(packet + 2) == 0); // Drops reported once, not once per packet.
    retained += n - 4;
  }
  assert(retained == DeviceLogBuffer::CAPACITY);

  buffer.append(flood.data(), flood.size());
  std::vector<uint8_t> overflow(70000, 'z');
  buffer.append(overflow.data(), overflow.size());
  assert(buffer.take(packet) == 20 && u16(packet + 2) == UINT16_MAX);
  buffer.reset(false);
  assert(buffer.take(packet) == 0);
  buffer.append(text, sizeof(text) - 1);
  buffer.reset(true);
  assert(buffer.take(packet) == 0); // Reconnect cannot replay the prior session.
  buffer.append(text, 1);
  assert(buffer.take(packet) == 5 && u16(packet) == 0 && u16(packet + 2) == 0);
}
