// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// MemTable: the in-memory, sorted write buffer in front of the SSTables.

#ifndef LSMKV_SRC_MEMTABLE_MEMTABLE_H_
#define LSMKV_SRC_MEMTABLE_MEMTABLE_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "lsmkv/iterator.h"
#include "memtable/skiplist.h"
#include "util/arena.h"

namespace lsmkv {

// A MemTable is split into N shards by hash(user_key). Each shard is an
// independent skiplist with its own arena and its own writer mutex:
//
//   * Writers to different shards never contend (after group commit, every
//     writer inserts its own batch in parallel).
//   * Readers take no lock at all (skiplist reads are lock-free), and a
//     point lookup touches exactly one shard, since a user key always
//     hashes to the same shard.
//   * Ordered iteration (flush, range scans) merges the N shard cursors
//     with a k-way heap merge.
//
// N = 1 degenerates to a single skiplist behind one mutex: the baseline
// that the scaling benchmark in BENCHMARKS.md compares against.
//
// Always owned through std::shared_ptr: iterators pin the memtable so it
// can be flushed and retired while a scan is still reading it.
class MemTable : public std::enable_shared_from_this<MemTable> {
 public:
  MemTable(const InternalKeyComparator& comparator, int num_shards);
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;
  ~MemTable();

  // Bytes of arena memory in use. Safe to call concurrently with writes.
  size_t ApproximateMemoryUsage() const;
  // Arena bytes used by entries, excluding the fixed per-shard cost of an
  // empty memtable (one arena block per shard for the skiplist head). This
  // is what write_buffer_size is compared against, so the flush threshold
  // means the same amount of data whatever the shard count.
  size_t ApproximateDataUsage() const {
    const size_t total = ApproximateMemoryUsage();
    return total > empty_usage_ ? total - empty_usage_ : 0;
  }
  uint64_t NumEntries() const {
    return num_entries_.load(std::memory_order_relaxed);
  }
  // Payload bytes (key + value) inserted, for bytes-per-key accounting.
  uint64_t RawDataBytes() const {
    return raw_bytes_.load(std::memory_order_relaxed);
  }
  int num_shards() const { return static_cast<int>(shards_.size()); }

  // Returns an iterator over internal keys in sorted order.
  std::unique_ptr<Iterator> NewIterator();

  // Adds an entry mapping key -> value at sequence number `seq` (value is
  // ignored for deletions). Thread-safe: locks only the owning shard.
  void Add(SequenceNumber seq, ValueType type, const Slice& key,
           const Slice& value);

  // If the memtable holds a value for key, stores it in *value and returns
  // true. If it holds a deletion for key, stores NotFound in *s and returns
  // true. Otherwise returns false. Lock-free.
  bool Get(const LookupKey& key, std::string* value, Status* s) const;

 private:
  // Orders arena entries, each of which starts with a length-prefixed
  // internal key.
  struct KeyComparator {
    const InternalKeyComparator* comparator;
    int operator()(const char* a, const char* b) const;
  };

  using Table = SkipList<const char*, KeyComparator>;

  // One cache line (at least) per shard so two shards' mutexes never share
  // a line (false sharing would re-introduce the contention we removed).
  struct alignas(64) Shard {
    explicit Shard(const KeyComparator& cmp, uint64_t seed)
        : table(cmp, &arena, seed) {}
    std::mutex mu;  // serializes writers of this shard only
    Arena arena;
    Table table;
  };

  size_t ShardIndex(const Slice& user_key) const;

  const InternalKeyComparator comparator_;
  const KeyComparator key_comparator_;
  std::vector<std::unique_ptr<Shard>> shards_;
  size_t empty_usage_ = 0;
  std::atomic<uint64_t> num_entries_{0};
  std::atomic<uint64_t> raw_bytes_{0};

  friend class MemTableIterator;
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_MEMTABLE_MEMTABLE_H_
