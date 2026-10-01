// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "util/arena.h"

#include <cstdint>

namespace lsmkv {

Arena::Arena() = default;

char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Large objects get a dedicated block so we do not waste the remainder
    // of the current block.
    return AllocateNewBlock(bytes);
  }
  // The remainder of the current block (< 1 KB here) is abandoned.
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateAligned(size_t bytes) {
  constexpr size_t align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
  static_assert((align & (align - 1)) == 0, "alignment must be a power of 2");
  const size_t current_mod =
      reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
  const size_t slop = (current_mod == 0 ? 0 : align - current_mod);
  const size_t needed = bytes + slop;
  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // new[] returns memory aligned for any fundamental type.
    result = AllocateFallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
  return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
  blocks_.emplace_back(new char[block_bytes]);
  memory_usage_.fetch_add(block_bytes + sizeof(std::unique_ptr<char[]>),
                          std::memory_order_relaxed);
  return blocks_.back().get();
}

}  // namespace lsmkv
