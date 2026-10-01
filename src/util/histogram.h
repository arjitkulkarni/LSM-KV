// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// HdrHistogram-style latency recorder for the benchmark harness.

#ifndef LSMKV_SRC_UTIL_HISTOGRAM_H_
#define LSMKV_SRC_UTIL_HISTOGRAM_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lsmkv {

// Log-linear buckets: values below 2^kSubBucketBits are exact; above that,
// every power of two is split into 2^kSubBucketBits equal sub-buckets. With
// 7 bits the worst-case relative error of any reported percentile is
// 1/128 = 0.78%, over a range of 1 ns .. ~73 min, in 37 KB -- the same
// trade-off HdrHistogram makes with 2 significant digits.
//
// Deliberately not thread-safe: each benchmark thread owns one and they are
// merged at the end, so recording is a single non-atomic increment.
class HdrHistogram {
 public:
  static constexpr int kSubBucketBits = 7;
  static constexpr int kSubBuckets = 1 << kSubBucketBits;
  static constexpr int kMaxExponent = 42;
  static constexpr int kNumBuckets =
      (kMaxExponent - kSubBucketBits + 1) * kSubBuckets + kSubBuckets;

  HdrHistogram();

  void Record(uint64_t value);
  void Merge(const HdrHistogram& other);
  void Clear();

  uint64_t Count() const { return count_; }
  uint64_t Min() const { return count_ == 0 ? 0 : min_; }
  uint64_t Max() const { return max_; }
  double Mean() const;
  // Highest value equivalent to the p-th percentile (0 < p <= 100): the
  // upper edge of the bucket holding that rank, i.e. conservative.
  uint64_t Percentile(double p) const;

  // (value, cumulative fraction) pairs, one per non-empty bucket; used to
  // draw the latency CDF.
  std::vector<std::pair<uint64_t, double>> Cdf() const;

  static int BucketFor(uint64_t v);
  static uint64_t BucketUpperBound(int index);

 private:
  std::vector<uint64_t> buckets_;
  uint64_t count_ = 0;
  uint64_t min_ = UINT64_MAX;
  uint64_t max_ = 0;
  long double sum_ = 0;
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_UTIL_HISTOGRAM_H_
