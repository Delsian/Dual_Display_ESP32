#pragma once
#include <stddef.h>
#include <stdint.h>

namespace BleOta {
enum State : uint8_t { Idle, Preparing, Receiving, Verifying, Complete, Error };
enum ErrorCode : uint8_t {
  None, Malformed, InvalidState, Bounds, Offset, Mtu, Partition,
  Flash, Hash, Image, Boot, Timeout, Aborted, Disconnected, Resource, Version
};
inline uint32_t image_size(const uint8_t *p) {
  return uint32_t(p[1]) | uint32_t(p[2]) << 8 |
         uint32_t(p[3]) << 16 | uint32_t(p[4]) << 24;
}
inline uint32_t data_offset(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
         uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline ErrorCode validate_data(const uint8_t *p, size_t size, uint16_t mtu,
                              uint32_t next, uint32_t total) {
  if (mtu < 40) return Mtu;
  if (size < 5 || size > 512 || size > size_t(mtu - 3)) return Malformed;
  if (data_offset(p) != next) return Offset;
  if (next > total || size - 4 > total - next) return Bounds;
  return None;
}
inline ErrorCode validate_control(const uint8_t *p, size_t size) {
  if (!size) return Malformed;
  if (p[0] == 1) return size == 37 ? None : Malformed;
  if (p[0] == 2 || p[0] == 3) return size == 1 ? None : Malformed;
  return Malformed;
}
}
