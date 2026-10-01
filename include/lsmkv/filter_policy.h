// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// FilterPolicy: a compact probabilistic summary of the keys in one SSTable.

#ifndef LSMKV_INCLUDE_LSMKV_FILTER_POLICY_H_
#define LSMKV_INCLUDE_LSMKV_FILTER_POLICY_H_

#include <memory>
#include <string>

#include "lsmkv/slice.h"

namespace lsmkv {

// A FilterPolicy lets a read skip an SSTable without touching its data
// blocks. False positives cost one wasted block read; false negatives would
// lose data, so KeyMayMatch must return true for every key that was passed
// to CreateFilter.
class FilterPolicy {
 public:
  FilterPolicy() = default;
  FilterPolicy(const FilterPolicy&) = delete;
  FilterPolicy& operator=(const FilterPolicy&) = delete;
  virtual ~FilterPolicy();

  // Persisted alongside the filter so an incompatible policy is detectable.
  virtual const char* Name() const = 0;

  // Appends a filter summarizing keys[0, n) to *dst.
  virtual void CreateFilter(const Slice* keys, int n,
                            std::string* dst) const = 0;

  // Returns false only if `key` was definitely not in the set.
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

// Classic Bloom filter with k = round(bits_per_key * ln 2) probes derived by
// double hashing from one 64-bit hash. bits_per_key = 10 gives ~1% false
// positives; see BENCHMARKS.md for the measured sweep.
std::shared_ptr<const FilterPolicy> NewBloomFilterPolicy(int bits_per_key);

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_FILTER_POLICY_H_
