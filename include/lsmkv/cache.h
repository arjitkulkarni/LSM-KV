// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Cache: a bounded, thread-safe key -> object map with an eviction policy.

#ifndef LSMKV_INCLUDE_LSMKV_CACHE_H_
#define LSMKV_INCLUDE_LSMKV_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "lsmkv/slice.h"

namespace lsmkv {

struct CacheStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t inserts = 0;
  uint64_t evictions = 0;
  size_t usage = 0;
  size_t capacity = 0;
};

// Values are type-erased shared_ptrs. Eviction only drops the cache's own
// reference, so a block that a reader is still iterating stays alive until
// that reader lets go -- no manual pin/unpin (Release) calls at call sites.
class Cache {
 public:
  Cache() = default;
  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;
  virtual ~Cache();

  // Inserts (or replaces) key -> value, charging `charge` bytes against the
  // capacity. May evict least-recently-used entries.
  virtual void Insert(const Slice& key, std::shared_ptr<void> value,
                      size_t charge) = 0;

  // Returns the cached value, or nullptr on a miss. A hit refreshes recency.
  virtual std::shared_ptr<void> Lookup(const Slice& key) = 0;

  virtual void Erase(const Slice& key) = 0;

  // A process-unique id. Clients sharing one cache use it as a key prefix so
  // their key spaces never collide (e.g. one id per open SSTable).
  virtual uint64_t NewId() = 0;

  virtual size_t TotalCharge() const = 0;
  virtual size_t Capacity() const = 0;
  virtual CacheStats GetStats() const = 0;
};

// Strict LRU with O(1) Lookup/Insert/Erase: an intrusive doubly-linked
// recency list plus a hash index, split into 2^num_shard_bits independently
// locked shards to cut lock contention. capacity == 0 disables caching.
std::shared_ptr<Cache> NewLRUCache(size_t capacity, int num_shard_bits = 4);

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_CACHE_H_
