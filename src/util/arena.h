// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Arena: a bump allocator for memtable nodes.

#ifndef LSMKV_SRC_UTIL_ARENA_H_
#define LSMKV_SRC_UTIL_ARENA_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

namespace lsmkv {

// Skiplist nodes and their key/value bytes are never freed individually:
// a memtable is written once, flushed, and then dropped as a whole. An
// arena turns ~2 malloc calls per insert into a pointer bump, packs nodes
// densely (better cache locality during search), and frees everything with
// one pass over a handful of 4 KB blocks.
//
// Not thread-safe for allocation: callers serialize Allocate() (the
// memtable shard mutex does). MemoryUsage() may be read concurrently.
class Arena {
 public:
  Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena() = default;

  // Returns a pointer to `bytes` of uninitialized memory (bytes > 0).
  char* Allocate(size_t bytes);
  // Like Allocate, aligned for any scalar type (alignof(max_align_t) or 8).
  char* AllocateAligned(size_t bytes);

  // Total bytes reserved from the system, including slack and bookkeeping.
  size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr size_t kBlockSize = 4096;

  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

inline char* Arena::Allocate(size_t bytes) {
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

}  // namespace lsmkv

#endif  // LSMKV_SRC_UTIL_ARENA_H_
