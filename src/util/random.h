// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_UTIL_RANDOM_H_
#define LSMKV_SRC_UTIL_RANDOM_H_

#include <cstdint>

namespace lsmkv {

// SplitMix64: tiny, fast, passes BigCrush, and fully deterministic for a
// given seed -- which is what makes randomized tests reproducible.
// Not thread-safe; give each thread its own instance.
class Random {
 public:
  explicit Random(uint64_t seed) : state_(seed) {}

  uint64_t Next() {
    uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }

  // Uniform in [0, n). The modulo bias is < n / 2^64, negligible here.
  uint64_t Uniform(uint64_t n) { return n == 0 ? 0 : Next() % n; }

  // True with probability 1/n.
  bool OneIn(uint64_t n) { return Uniform(n) == 0; }

  // Uniform in [0, 1).
  double NextDouble() {
    return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0);
  }

 private:
  uint64_t state_;
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_UTIL_RANDOM_H_
