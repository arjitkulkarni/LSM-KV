// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "lsmkv/comparator.h"

#include <algorithm>
#include <cstdint>

namespace lsmkv {

Comparator::~Comparator() = default;

namespace {

class BytewiseComparatorImpl final : public Comparator {
 public:
  const char* Name() const override { return "lsmkv.BytewiseComparator"; }

  int Compare(const Slice& a, const Slice& b) const override {
    return a.compare(b);
  }

  // "abcdefg" vs "abzzz" -> "abd": the shortest string that still sorts
  // after every key in the block and before every key in the next one.
  void FindShortestSeparator(std::string* start,
                             const Slice& limit) const override {
    const size_t min_length = std::min(start->size(), limit.size());
    size_t diff_index = 0;
    while (diff_index < min_length &&
           (*start)[diff_index] == limit[diff_index]) {
      diff_index++;
    }
    if (diff_index >= min_length) {
      return;  // one is a prefix of the other; leave it alone
    }
    const auto diff_byte = static_cast<uint8_t>((*start)[diff_index]);
    if (diff_byte < 0xff &&
        diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
      (*start)[diff_index]++;
      start->resize(diff_index + 1);
    }
  }

  void FindShortSuccessor(std::string* key) const override {
    for (size_t i = 0; i < key->size(); i++) {
      const auto byte = static_cast<uint8_t>((*key)[i]);
      if (byte != 0xff) {
        (*key)[i] = static_cast<char>(byte + 1);
        key->resize(i + 1);
        return;
      }
    }
    // *key is a run of 0xffs; leave it alone.
  }
};

}  // namespace

const Comparator* BytewiseComparator() {
  static const BytewiseComparatorImpl singleton;
  return &singleton;
}

}  // namespace lsmkv
