// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "util/arena.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "util/random.h"

namespace lsmkv {

TEST(ArenaTest, Empty) {
  Arena arena;
  EXPECT_EQ(0u, arena.MemoryUsage());
}

TEST(ArenaTest, SimpleAllocationsAreDisjointAndIntact) {
  std::vector<std::pair<size_t, char*>> allocated;
  Arena arena;
  const int N = 100000;
  size_t bytes = 0;
  Random rnd(301);
  for (int i = 0; i < N; i++) {
    size_t s;
    if (i % (N / 10) == 0) {
      s = static_cast<size_t>(i);
    } else {
      s = rnd.OneIn(4000)
              ? rnd.Uniform(6000)
              : (rnd.OneIn(10) ? rnd.Uniform(100) : rnd.Uniform(20));
    }
    if (s == 0) s = 1;  // Our arena disallows size 0 allocations.
    char* r = rnd.OneIn(10) ? arena.AllocateAligned(s) : arena.Allocate(s);

    for (size_t b = 0; b < s; b++) {
      // Fill the "i"th allocation with a known bit pattern
      r[b] = static_cast<char>(i % 256);
    }
    bytes += s;
    allocated.emplace_back(s, r);
    ASSERT_GE(arena.MemoryUsage(), bytes);
    if (i > N / 10) {
      // Overhead (block slack) stays bounded.
      ASSERT_LE(static_cast<double>(arena.MemoryUsage()), bytes * 1.10);
    }
  }
  for (size_t i = 0; i < allocated.size(); i++) {
    const size_t num_bytes = allocated[i].first;
    const char* p = allocated[i].second;
    for (size_t b = 0; b < num_bytes; b++) {
      ASSERT_EQ(static_cast<int>(p[b]) & 0xff, static_cast<int>(i % 256));
    }
  }
}

TEST(ArenaTest, AlignedAllocationsAreAligned) {
  Arena arena;
  for (int i = 1; i < 2000; i++) {
    arena.Allocate(static_cast<size_t>(i % 7 + 1));  // misalign on purpose
    char* p = arena.AllocateAligned(static_cast<size_t>(i % 100 + 1));
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % 8);
  }
}

}  // namespace lsmkv
