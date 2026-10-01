// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "util/histogram.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "lsmkv/metrics.h"
#include "util/random.h"

namespace lsmkv {

namespace {

uint64_t ExactPercentile(std::vector<uint64_t> v, double p) {
  std::sort(v.begin(), v.end());
  auto rank = static_cast<size_t>(std::ceil(p / 100.0 * v.size()));
  if (rank < 1) rank = 1;
  return v[rank - 1];
}

}  // namespace

TEST(HdrHistogramTest, BucketsAreContiguousAndMonotone) {
  uint64_t prev_upper = 0;
  for (int i = 1; i < HdrHistogram::kNumBuckets; i++) {
    const uint64_t upper = HdrHistogram::BucketUpperBound(i);
    ASSERT_GT(upper, prev_upper) << i;
    // The value one past the previous bucket's top maps into this bucket.
    ASSERT_EQ(i, HdrHistogram::BucketFor(prev_upper + 1)) << i;
    ASSERT_EQ(i, HdrHistogram::BucketFor(upper)) << i;
    prev_upper = upper;
  }
}

TEST(HdrHistogramTest, PercentilesWithinOnePercentOfExact) {
  Random rnd(99);
  HdrHistogram h;
  std::vector<uint64_t> values;
  for (int i = 0; i < 200000; i++) {
    // Heavy-tailed latencies: mostly microseconds, occasionally milliseconds.
    uint64_t v = 500 + rnd.Uniform(5000);
    if (rnd.OneIn(100)) v = 1000000 + rnd.Uniform(50000000);
    values.push_back(v);
    h.Record(v);
  }
  for (double p : {50.0, 90.0, 99.0, 99.9, 99.99, 100.0}) {
    const double exact = static_cast<double>(ExactPercentile(values, p));
    const double approx = static_cast<double>(h.Percentile(p));
    EXPECT_GE(approx, exact) << p;  // conservative: never under-reports
    EXPECT_LE(approx, exact * 1.0079) << p;
  }
  EXPECT_EQ(200000u, h.Count());
}

TEST(HdrHistogramTest, MergeEqualsRecordingEverything) {
  HdrHistogram a;
  HdrHistogram b;
  HdrHistogram both;
  for (uint64_t v = 1; v < 100000; v += 7) {
    ((v % 2) ? a : b).Record(v);
    both.Record(v);
  }
  a.Merge(b);
  for (double p : {1.0, 50.0, 99.0}) EXPECT_EQ(both.Percentile(p), a.Percentile(p));
  EXPECT_EQ(both.Count(), a.Count());
  EXPECT_EQ(both.Max(), a.Max());
}

TEST(MetricsHistogramTest, PercentilesWithinBucketError) {
  Histogram h("x", {});
  Random rnd(5);
  std::vector<uint64_t> values;
  for (int i = 0; i < 100000; i++) {
    const uint64_t v = 1000 + rnd.Uniform(1000000);
    values.push_back(v);
    h.Record(v);
  }
  for (double p : {50.0, 99.0, 99.9}) {
    const double exact = static_cast<double>(ExactPercentile(values, p));
    const double approx = static_cast<double>(h.PercentileNanos(p));
    EXPECT_GE(approx, exact) << p;
    EXPECT_LE(approx, exact * (1.0 + 1.0 / Histogram::kSubBuckets)) << p;
  }
}

}  // namespace lsmkv
