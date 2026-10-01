// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "lsmkv/filter_policy.h"
#include "util/coding.h"

namespace lsmkv {

namespace {

Slice IntKey(int i, char* buffer) {
  EncodeFixed32(buffer, static_cast<uint32_t>(i));
  return Slice(buffer, sizeof(uint32_t));
}

class BloomTest {
 public:
  explicit BloomTest(int bits) : policy_(NewBloomFilterPolicy(bits)) {}

  void Add(const Slice& s) { keys_.push_back(s.ToString()); }

  void Build() {
    std::vector<Slice> key_slices;
    for (const auto& k : keys_) key_slices.emplace_back(k);
    filter_.clear();
    policy_->CreateFilter(key_slices.data(), static_cast<int>(key_slices.size()),
                          &filter_);
    keys_.clear();
  }

  size_t FilterSize() const { return filter_.size(); }

  bool Matches(const Slice& s) {
    if (!keys_.empty()) Build();
    return policy_->KeyMayMatch(s, filter_);
  }

  double FalsePositiveRate() {
    char buffer[sizeof(int)];
    int result = 0;
    for (int i = 0; i < 10000; i++) {
      if (Matches(IntKey(i + 1000000000, buffer))) result++;
    }
    return result / 10000.0;
  }

 private:
  std::shared_ptr<const FilterPolicy> policy_;
  std::string filter_;
  std::vector<std::string> keys_;
};

int NextLength(int length) {
  if (length < 10) return length + 1;
  if (length < 100) return length + 10;
  if (length < 1000) return length + 100;
  return length + 1000;
}

}  // namespace

TEST(BloomTest, EmptyFilter) {
  BloomTest t(10);
  EXPECT_FALSE(t.Matches("hello"));
  EXPECT_FALSE(t.Matches("world"));
}

TEST(BloomTest, Small) {
  BloomTest t(10);
  t.Add("hello");
  t.Add("world");
  EXPECT_TRUE(t.Matches("hello"));
  EXPECT_TRUE(t.Matches("world"));
  EXPECT_FALSE(t.Matches("x"));
  EXPECT_FALSE(t.Matches("foo"));
}

TEST(BloomTest, NoFalseNegativesAndBoundedFalsePositives) {
  char buffer[sizeof(int)];
  int mediocre_filters = 0;
  int good_filters = 0;
  for (int length = 1; length <= 10000; length = NextLength(length)) {
    BloomTest t(10);
    for (int i = 0; i < length; i++) t.Add(IntKey(i, buffer));
    t.Build();
    EXPECT_LE(t.FilterSize(), static_cast<size_t>((length * 10 / 8) + 40)) << length;
    // All added keys must match: a false negative would lose data.
    for (int i = 0; i < length; i++) {
      ASSERT_TRUE(t.Matches(IntKey(i, buffer))) << "Length " << length << "; key " << i;
    }
    const double rate = t.FalsePositiveRate();
    EXPECT_LE(rate, 0.02) << length;  // Must not be over 2%
    if (rate > 0.0125) {
      mediocre_filters++;  // Allowed, but not too often
    } else {
      good_filters++;
    }
  }
  EXPECT_LE(mediocre_filters, good_filters / 5);
}

TEST(BloomTest, FalsePositiveRateTracksTheory) {
  // (1 - e^{-kn/m})^k with k = floor(0.69 * bits); generous tolerance.
  char buffer[sizeof(int)];
  for (int bits : {4, 6, 8, 10, 14}) {
    BloomTest t(bits);
    for (int i = 0; i < 20000; i++) t.Add(IntKey(i, buffer));
    t.Build();
    const int k = std::max(1, static_cast<int>(bits * 0.69));
    const double theory = std::pow(1.0 - std::exp(-static_cast<double>(k) / bits), k);
    const double measured = t.FalsePositiveRate();
    EXPECT_LT(measured, theory * 1.6 + 0.002) << bits << " bits/key";
    EXPECT_GT(measured, theory * 0.4) << bits << " bits/key";
  }
}

}  // namespace lsmkv
