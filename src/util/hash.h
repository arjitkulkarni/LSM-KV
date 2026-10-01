// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_UTIL_HASH_H_
#define LSMKV_SRC_UTIL_HASH_H_

#include <cstddef>
#include <cstdint>

#include "lsmkv/slice.h"

namespace lsmkv {

// 64-bit MurmurHash2 (MurmurHash64A). Fast, well-distributed, and stable
// across platforms, so Bloom filters written on one machine read correctly
// on another. Not cryptographic.
uint64_t Hash64(const char* data, size_t n, uint64_t seed);

inline uint64_t Hash64(const Slice& s, uint64_t seed = 0x9ae16a3b2f90404full) {
  return Hash64(s.data(), s.size(), seed);
}

}  // namespace lsmkv

#endif  // LSMKV_SRC_UTIL_HASH_H_
