// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "util/hash.h"

#include <cstring>

namespace lsmkv {

uint64_t Hash64(const char* data, size_t n, uint64_t seed) {
  constexpr uint64_t m = 0xc6a4a7935bd1e995ull;
  constexpr int r = 47;

  uint64_t h = seed ^ (n * m);
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = p + (n / 8) * 8;

  while (p != end) {
    uint64_t k;
    std::memcpy(&k, p, sizeof(k));  // little-endian hosts only (x86/ARM)
    p += 8;
    k *= m;
    k ^= k >> r;
    k *= m;
    h ^= k;
    h *= m;
  }

  switch (n & 7) {
    case 7: h ^= static_cast<uint64_t>(p[6]) << 48; [[fallthrough]];
    case 6: h ^= static_cast<uint64_t>(p[5]) << 40; [[fallthrough]];
    case 5: h ^= static_cast<uint64_t>(p[4]) << 32; [[fallthrough]];
    case 4: h ^= static_cast<uint64_t>(p[3]) << 24; [[fallthrough]];
    case 3: h ^= static_cast<uint64_t>(p[2]) << 16; [[fallthrough]];
    case 2: h ^= static_cast<uint64_t>(p[1]) << 8; [[fallthrough]];
    case 1:
      h ^= static_cast<uint64_t>(p[0]);
      h *= m;
      break;
    default:
      break;
  }

  h ^= h >> r;
  h *= m;
  h ^= h >> r;
  return h;
}

}  // namespace lsmkv
