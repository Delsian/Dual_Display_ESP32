#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Caller serializes access. No allocation or waiting in the producer path.
class DeviceLogBuffer {
public:
  static constexpr size_t CAPACITY = 2048;
  static constexpr size_t PACKET_SIZE = 20;

  void reset(bool enabled) {
    active = enabled;
    head = used = 0;
    sequence = dropped = 0;
  }

  void append(const uint8_t *data, size_t size) {
    if (!active) return;
    const size_t accepted = size < CAPACITY - used ? size : CAPACITY - used;
    const size_t tail = (head + used) % CAPACITY;
    const size_t first = accepted < CAPACITY - tail ? accepted : CAPACITY - tail;
    memcpy(bytes + tail, data, first);
    memcpy(bytes, data + first, accepted - first);
    used += accepted;
    const size_t lost = size - accepted;
    dropped = lost >= size_t(UINT16_MAX - dropped) ? UINT16_MAX : dropped + lost;
  }

  size_t take(uint8_t *packet) {
    if (!active || !used) return 0;
    packet[0] = sequence & 0xff;
    packet[1] = sequence >> 8;
    packet[2] = dropped & 0xff;
    packet[3] = dropped >> 8;
    ++sequence;
    dropped = 0;
    const size_t count = used < PACKET_SIZE - 4 ? used : PACKET_SIZE - 4;
    for (size_t i = 0; i < count; ++i) packet[4 + i] = bytes[(head + i) % CAPACITY];
    head = (head + count) % CAPACITY;
    used -= count;
    return count + 4;
  }

private:
  bool active = false;
  uint8_t bytes[CAPACITY] = {};
  size_t head = 0, used = 0;
  uint16_t sequence = 0, dropped = 0;
};
