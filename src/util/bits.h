// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Log-linear bucketing shared by the benchmark histogram and the metrics
// histogram.

#ifndef LSMKV_SRC_UTIL_BITS_H_
#define LSMKV_SRC_UTIL_BITS_H_

#include <cstdint>

namespace lsmkv {

inline int HighestBit(uint64_t v) {  // REQUIRES: v != 0
#if defined(__GNUC__) || defined(__clang__)
  return 63 - __builtin_clzll(v);
#else
  int r = 0;
  while (v >>= 1) r++;
  return r;
#endif
}

// Maps v to a bucket index. Bucket layout for kBits = 7:
//   [0, 128)            one bucket per value (exact)
//   [128, 256)          width 1  -> indices 128..255
//   [256, 512)          width 2  -> indices 256..383, ...
// Values >= 2^(kMaxExp+1) are clamped into the last bucket.
template <int kBits, int kMaxExp>
inline int LogLinearBucket(uint64_t v) {
  constexpr uint64_t kSub = uint64_t{1} << kBits;
  if (v < kSub) return static_cast<int>(v);
  constexpr uint64_t kClamp = (uint64_t{1} << (kMaxExp + 1)) - 1;
  if (v > kClamp) v = kClamp;
  const int msb = HighestBit(v);
  const int shift = msb - kBits;
  return (shift + 1) * static_cast<int>(kSub) +
         static_cast<int>((v >> shift) - kSub);
}

template <int kBits>
inline uint64_t LogLinearUpperBound(int index) {
  constexpr int kSub = 1 << kBits;
  if (index < kSub) return static_cast<uint64_t>(index);
  const int shift = index / kSub - 1;
  const uint64_t sub = static_cast<uint64_t>(index % kSub + kSub);
  return ((sub + 1) << shift) - 1;
}

}  // namespace lsmkv

#endif  // LSMKV_SRC_UTIL_BITS_H_
