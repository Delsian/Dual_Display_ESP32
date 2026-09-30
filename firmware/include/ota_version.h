#pragma once
#include <stdint.h>
#include <stddef.h>

// Canonical numeric releases only: no leading zeros, suffixes or overflow.
inline bool parse_ota_version(const char *text, size_t size, uint32_t (&v)[3]) {
  size_t pos = 0;
  for (unsigned part = 0; part < 3; ++part) {
    if (pos >= size || text[pos] < '0' || text[pos] > '9') return false;
    const size_t first = pos;
    v[part] = 0;
    while (pos < size && text[pos] >= '0' && text[pos] <= '9') {
      const unsigned digit = text[pos++] - '0';
      if (v[part] > (UINT32_MAX - digit) / 10) return false;
      v[part] = v[part] * 10 + digit;
    }
    if (pos - first > 1 && text[first] == '0') return false;
    if (part < 2) { if (pos >= size || text[pos++] != '.') return false; }
  }
  return pos < size && text[pos] == '\0';
}

inline bool ota_version_higher(const char *candidate, size_t size, const char *current, size_t current_size) {
  uint32_t incoming[3], running[3];
  if (!parse_ota_version(candidate, size, incoming) || !parse_ota_version(current, current_size, running)) return false;
  for (unsigned i = 0; i < 3; ++i) {
    if (incoming[i] != running[i]) return incoming[i] > running[i];
  }
  return false;
}
