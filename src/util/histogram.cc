// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "util/histogram.h"

#include <algorithm>
#include <cmath>

#include "util/bits.h"

namespace lsmkv {

HdrHistogram::HdrHistogram() : buckets_(kNumBuckets, 0) {}

int HdrHistogram::BucketFor(uint64_t v) {
  return LogLinearBucket<kSubBucketBits, kMaxExponent>(v);
}

uint64_t HdrHistogram::BucketUpperBound(int index) {
  return LogLinearUpperBound<kSubBucketBits>(index);
}

void HdrHistogram::Record(uint64_t value) {
  buckets_[static_cast<size_t>(BucketFor(value))]++;
  count_++;
  min_ = std::min(min_, value);
  max_ = std::max(max_, value);
  sum_ += static_cast<long double>(value);
}

void HdrHistogram::Merge(const HdrHistogram& other) {
  for (int i = 0; i < kNumBuckets; i++) {
    buckets_[static_cast<size_t>(i)] += other.buckets_[static_cast<size_t>(i)];
  }
  count_ += other.count_;
  min_ = std::min(min_, other.min_);
  max_ = std::max(max_, other.max_);
  sum_ += other.sum_;
}

void HdrHistogram::Clear() {
  std::fill(buckets_.begin(), buckets_.end(), 0);
  count_ = 0;
  min_ = UINT64_MAX;
  max_ = 0;
  sum_ = 0;
}

double HdrHistogram::Mean() const {
  return count_ == 0 ? 0.0 : static_cast<double>(sum_ / count_);
}

uint64_t HdrHistogram::Percentile(double p) const {
  if (count_ == 0) return 0;
  p = std::clamp(p, 0.0, 100.0);
  // Rank of the requested percentile, 1-based, rounded up (nearest-rank).
  auto rank = static_cast<uint64_t>(
      std::ceil(p / 100.0 * static_cast<double>(count_)));
  rank = std::clamp<uint64_t>(rank, 1, count_);
  uint64_t cumulative = 0;
  for (int i = 0; i < kNumBuckets; i++) {
    cumulative += buckets_[static_cast<size_t>(i)];
    if (cumulative >= rank) {
      // Never report more than the true maximum.
      return std::min(BucketUpperBound(i), max_);
    }
  }
  return max_;
}

std::vector<std::pair<uint64_t, double>> HdrHistogram::Cdf() const {
  std::vector<std::pair<uint64_t, double>> out;
  if (count_ == 0) return out;
  uint64_t cumulative = 0;
  for (int i = 0; i < kNumBuckets; i++) {
    const uint64_t c = buckets_[static_cast<size_t>(i)];
    if (c == 0) continue;
    cumulative += c;
    out.emplace_back(std::min(BucketUpperBound(i), max_),
                     static_cast<double>(cumulative) /
                         static_cast<double>(count_));
  }
  return out;
}

}  // namespace lsmkv
