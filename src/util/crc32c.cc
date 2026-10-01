// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Portable slicing-by-8 CRC-32C. The eight 256-entry tables are generated at
// compile time, so there is no static-initialization-order hazard and no
// runtime setup cost.

#include "util/crc32c.h"

#include <array>

namespace lsmkv::crc32c {
namespace {

constexpr uint32_t kPoly = 0x82f63b78u;  // reflected 0x1EDC6F41

using Table = std::array<std::array<uint32_t, 256>, 8>;

constexpr Table MakeTables() {
  Table t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int k = 0; k < 8; ++k) {
      crc = (crc & 1) ? (crc >> 1) ^ kPoly : (crc >> 1);
    }
    t[0][i] = crc;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    for (int s = 1; s < 8; ++s) {
      t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xff];
    }
  }
  return t;
}

constexpr Table kTables = MakeTables();

inline uint32_t LoadLE32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  uint32_t crc = init_crc ^ 0xffffffffu;

  while (n >= 8) {
    const uint32_t lo = LoadLE32(p) ^ crc;
    const uint32_t hi = LoadLE32(p + 4);
    crc = kTables[7][lo & 0xff] ^ kTables[6][(lo >> 8) & 0xff] ^
          kTables[5][(lo >> 16) & 0xff] ^ kTables[4][lo >> 24] ^
          kTables[3][hi & 0xff] ^ kTables[2][(hi >> 8) & 0xff] ^
          kTables[1][(hi >> 16) & 0xff] ^ kTables[0][hi >> 24];
    p += 8;
    n -= 8;
  }
  while (n > 0) {
    crc = kTables[0][(crc ^ *p) & 0xff] ^ (crc >> 8);
    ++p;
    --n;
  }
  return crc ^ 0xffffffffu;
}

}  // namespace lsmkv::crc32c
