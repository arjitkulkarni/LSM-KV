// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "lsmkv/cache.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "util/coding.h"

namespace lsmkv {

namespace {

std::string EncodeKey(int k) {
  std::string result;
  PutFixed32(&result, static_cast<uint32_t>(k));
  return result;
}

std::shared_ptr<void> Val(int v) { return std::make_shared<int>(v); }

int Get(Cache* cache, int key) {
  auto v = cache->Lookup(EncodeKey(key));
  return v == nullptr ? -1 : *std::static_pointer_cast<int>(v);
}

}  // namespace

TEST(CacheTest, HitAndMiss) {
  auto cache = NewLRUCache(1000, 0);
  EXPECT_EQ(-1, Get(cache.get(), 100));

  cache->Insert(EncodeKey(100), Val(101), 1);
  EXPECT_EQ(101, Get(cache.get(), 100));
  EXPECT_EQ(-1, Get(cache.get(), 200));

  cache->Insert(EncodeKey(200), Val(201), 1);
  EXPECT_EQ(101, Get(cache.get(), 100));
  EXPECT_EQ(201, Get(cache.get(), 200));

  cache->Insert(EncodeKey(100), Val(102), 1);  // replace
  EXPECT_EQ(102, Get(cache.get(), 100));

  const CacheStats s = cache->GetStats();
  EXPECT_EQ(4u, s.hits);
  EXPECT_EQ(2u, s.misses);
}

TEST(CacheTest, Erase) {
  auto cache = NewLRUCache(1000, 0);
  cache->Erase(EncodeKey(200));
  cache->Insert(EncodeKey(100), Val(101), 1);
  cache->Insert(EncodeKey(200), Val(201), 1);
  cache->Erase(EncodeKey(100));
  EXPECT_EQ(-1, Get(cache.get(), 100));
  EXPECT_EQ(201, Get(cache.get(), 200));
}

TEST(CacheTest, EvictsLeastRecentlyUsed) {
  auto cache = NewLRUCache(3, 0);
  cache->Insert(EncodeKey(1), Val(1), 1);
  cache->Insert(EncodeKey(2), Val(2), 1);
  cache->Insert(EncodeKey(3), Val(3), 1);
  EXPECT_EQ(1, Get(cache.get(), 1));  // 1 is now most recent; 2 is LRU
  cache->Insert(EncodeKey(4), Val(4), 1);
  EXPECT_EQ(-1, Get(cache.get(), 2));
  EXPECT_EQ(1, Get(cache.get(), 1));
  EXPECT_EQ(3, Get(cache.get(), 3));
  EXPECT_EQ(4, Get(cache.get(), 4));
  EXPECT_EQ(3u, cache->TotalCharge());
}

TEST(CacheTest, HeavyEntriesAreChargedByWeight) {
  auto cache = NewLRUCache(1000, 0);
  // 100 light entries then one entry weighing 950.
  for (int i = 0; i < 100; i++) cache->Insert(EncodeKey(i), Val(i), 1);
  cache->Insert(EncodeKey(1000), Val(1000), 950);
  EXPECT_LE(cache->TotalCharge(), 1000u);
  EXPECT_EQ(1000, Get(cache.get(), 1000));
}

TEST(CacheTest, EvictedValueStaysAliveWhileReferenced) {
  // The property the block cache relies on: eviction never frees a block a
  // reader is still using.
  auto cache = NewLRUCache(1, 0);
  cache->Insert(EncodeKey(1), Val(11), 1);
  std::shared_ptr<void> pinned = cache->Lookup(EncodeKey(1));
  cache->Insert(EncodeKey(2), Val(22), 1);  // evicts key 1
  EXPECT_EQ(-1, Get(cache.get(), 1));
  EXPECT_EQ(11, *std::static_pointer_cast<int>(pinned));  // still valid
}

TEST(CacheTest, ZeroCapacityDisablesCaching) {
  auto cache = NewLRUCache(0, 0);
  cache->Insert(EncodeKey(1), Val(1), 1);
  EXPECT_EQ(-1, Get(cache.get(), 1));
}

TEST(CacheTest, NewIdIsUnique) {
  auto cache = NewLRUCache(10);
  const uint64_t a = cache->NewId();
  const uint64_t b = cache->NewId();
  EXPECT_NE(a, b);
}

TEST(CacheTest, ConcurrentAccessIsSafe) {
  auto cache = NewLRUCache(500, 4);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&cache, t] {
      for (int i = 0; i < 20000; i++) {
        const int k = (i * 7 + t) % 1000;
        if (i % 3 == 0) {
          cache->Insert(EncodeKey(k), Val(k), 1);
        } else {
          const int v = Get(cache.get(), k);
          ASSERT_TRUE(v == -1 || v == k);
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_LE(cache->TotalCharge(), 500u + 16);  // per-shard rounding
}

}  // namespace lsmkv
