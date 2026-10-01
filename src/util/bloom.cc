// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Bloom filter. Layout of an encoded filter:
//
//   [ bit array : ceil(n * bits_per_key / 8) bytes ][ k : 1 byte ]
//
// Probes use Kirsch-Mitzenmacher double hashing: g_i(x) = h1 + i*h2, with
// h1/h2 the two halves of one 64-bit hash. This matches the false-positive
// rate of k independent hashes while hashing the key only once.

#include <algorithm>
#include <cstdint>

#include "lsmkv/filter_policy.h"
#include "util/hash.h"

namespace lsmkv {

FilterPolicy::~FilterPolicy() = default;

namespace {

class BloomFilterPolicy final : public FilterPolicy {
 public:
  explicit BloomFilterPolicy(int bits_per_key)
      : bits_per_key_(std::max(1, bits_per_key)) {
    // k = bits_per_key * ln(2) minimizes the false-positive probability.
    k_ = static_cast<int>(bits_per_key_ * 0.69);
    k_ = std::clamp(k_, 1, 30);
  }

  const char* Name() const override { return "lsmkv.BuiltinBloomFilter"; }

  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
    // Tiny filters have a very high FP rate from rounding; enforce 64 bits.
    size_t bits = static_cast<size_t>(n) * static_cast<size_t>(bits_per_key_);
    bits = std::max<size_t>(bits, 64);
    const size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    dst->push_back(static_cast<char>(k_));
    char* array = &(*dst)[init_size];
    for (int i = 0; i < n; i++) {
      const uint64_t h = Hash64(keys[i]);
      uint32_t h1 = static_cast<uint32_t>(h);
      const uint32_t h2 = static_cast<uint32_t>(h >> 32) | 1u;  // odd step
      for (int j = 0; j < k_; j++) {
        const size_t bitpos = h1 % bits;
        array[bitpos / 8] =
            static_cast<char>(array[bitpos / 8] | (1 << (bitpos % 8)));
        h1 += h2;
      }
    }
  }

  bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const override {
    const size_t len = bloom_filter.size();
    if (len < 2) return false;

    const char* array = bloom_filter.data();
    const size_t bits = (len - 1) * 8;

    const int k = static_cast<uint8_t>(array[len - 1]);
    if (k > 30) {
      // Reserved for future encodings; treat as a match so we never lose data.
      return true;
    }

    const uint64_t h = Hash64(key);
    uint32_t h1 = static_cast<uint32_t>(h);
    const uint32_t h2 = static_cast<uint32_t>(h >> 32) | 1u;
    for (int j = 0; j < k; j++) {
      const size_t bitpos = h1 % bits;
      if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
      h1 += h2;
    }
    return true;
  }

 private:
  int bits_per_key_;
  int k_;
};

}  // namespace

std::shared_ptr<const FilterPolicy> NewBloomFilterPolicy(int bits_per_key) {
  return std::make_shared<BloomFilterPolicy>(bits_per_key);
}

}  // namespace lsmkv
