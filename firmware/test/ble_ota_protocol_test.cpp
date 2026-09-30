#include "ble_ota_protocol.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>

int main() {
  using namespace BleOta;
  uint8_t packet[512] = {};
  assert(validate_control(nullptr, 0) == Malformed);
  for (unsigned opcode = 0; opcode < 256; ++opcode) {
    packet[0] = opcode;
    for (size_t size = 1; size <= sizeof(packet); ++size) {
      const bool valid = (opcode == 1 && size == 37) ||
          ((opcode == 2 || opcode == 3) && size == 1);
      assert((validate_control(packet, size) == None) == valid);
    }
  }
  packet[1] = 0x78; packet[2] = 0x56; packet[3] = 0x34; packet[4] = 0x12;
  assert(image_size(packet) == 0x12345678u);
  packet[1] = packet[2] = packet[3] = packet[4] = 0xff;
  assert(image_size(packet) == UINT32_MAX);
  packet[1] = packet[2] = packet[3] = packet[4] = 0;
  assert(image_size(packet) == 0);
  packet[0] = packet[1] = packet[2] = packet[3] = 0;
  assert(validate_data(nullptr, 0, 185, 0, 1000) == Malformed);
  assert(validate_data(packet, 4, 185, 0, 1000) == Malformed);
  assert(validate_data(packet, 5, 23, 0, 1000) == Mtu);
  assert(validate_data(packet, 182, 185, 0, 1000) == None);
  assert(validate_data(packet, 183, 185, 0, 1000) == Malformed);
  assert(validate_data(packet, 512, 517, 0, 1000) == None);
  assert(validate_data(packet, 513, 517, 0, 1000) == Malformed);
  assert(validate_data(packet, 5, 185, 0, 0) == Bounds);
  assert(validate_data(packet, 5, 185, 1, 1000) == Offset);
  // Two full chunks and a final one-byte chunk; a duplicate never advances.
  uint32_t next = 0;
  const uint32_t total = 357;
  for (size_t length : {size_t(178), size_t(178), size_t(1)}) {
    for (unsigned i = 0; i < 4; ++i) packet[i] = uint8_t(next >> (8 * i));
    assert(validate_data(packet, length + 4, 185, next, total) == None);
    const uint32_t advanced = next + length;
    assert(validate_data(packet, length + 4, 185, advanced, total) == Offset);
    next = advanced;
  }
  for (unsigned i = 0; i < 4; ++i) packet[i] = uint8_t(next >> (8 * i));
  assert(validate_data(packet, 5, 185, next, total) == Bounds);
  // Subtraction-based bounds checks must reject wraparound sizes/offsets.
  packet[0] = packet[1] = packet[2] = packet[3] = 0xff;
  assert(validate_data(packet, 5, 185, UINT32_MAX, UINT32_MAX) == Bounds);
  assert(validate_data(packet, 5, 185, UINT32_MAX, 10) == Bounds);
}
