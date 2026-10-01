// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Comparator: the policy that defines the total order of keys.

#ifndef LSMKV_INCLUDE_LSMKV_COMPARATOR_H_
#define LSMKV_INCLUDE_LSMKV_COMPARATOR_H_

#include <string>

#include "lsmkv/slice.h"

namespace lsmkv {

// Strategy interface for key ordering. Every sorted structure in the engine
// (skiplist, SSTable blocks, index, level metadata) is parameterized by a
// Comparator, so a user can swap in a different order without touching the
// storage code. Implementations must be thread-safe and stateless in effect.
class Comparator {
 public:
  Comparator() = default;
  Comparator(const Comparator&) = delete;
  Comparator& operator=(const Comparator&) = delete;
  virtual ~Comparator();

  // Three-way comparison: <0 iff a<b, 0 iff a==b, >0 iff a>b.
  virtual int Compare(const Slice& a, const Slice& b) const = 0;

  // Persisted in the MANIFEST; opening a DB with a comparator of a different
  // name is refused, because the on-disk order would be meaningless.
  virtual const char* Name() const = 0;

  // If *start < limit, may shorten *start to any string in [*start, limit).
  // Used to keep SSTable index entries small. A no-op is always correct.
  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const = 0;

  // May change *key to any string >= *key. A no-op is always correct.
  virtual void FindShortSuccessor(std::string* key) const = 0;
};

// Unsigned lexicographic byte order. The returned object has static storage
// duration and must not be deleted.
const Comparator* BytewiseComparator();

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_COMPARATOR_H_
