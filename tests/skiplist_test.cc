// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "memtable/skiplist.h"

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "util/arena.h"
#include "util/random.h"

namespace lsmkv {

using Key = uint64_t;

struct TestComparator {
  int operator()(const Key& a, const Key& b) const {
    if (a < b) return -1;
    if (a > b) return +1;
    return 0;
  }
};

TEST(SkipListTest, Empty) {
  Arena arena;
  TestComparator cmp;
  SkipList<Key, TestComparator> list(cmp, &arena);
  EXPECT_FALSE(list.Contains(10));

  SkipList<Key, TestComparator>::Iterator iter(&list);
  EXPECT_FALSE(iter.Valid());
  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());
  iter.Seek(100);
  EXPECT_FALSE(iter.Valid());
}

TEST(SkipListTest, InsertAndLookupMatchesStdSet) {
  const int N = 2000;
  const int R = 5000;
  Random rnd(1000);
  std::set<Key> keys;
  Arena arena;
  TestComparator cmp;
  SkipList<Key, TestComparator> list(cmp, &arena);
  for (int i = 0; i < N; i++) {
    const Key key = rnd.Uniform(R);
    if (keys.insert(key).second) list.Insert(key);
  }

  for (Key i = 0; i < static_cast<Key>(R); i++) {
    EXPECT_EQ(keys.count(i) == 1, list.Contains(i)) << i;
  }

  // Forward iteration yields exactly the set, in order.
  {
    SkipList<Key, TestComparator>::Iterator iter(&list);
    iter.SeekToFirst();
    for (Key k : keys) {
      ASSERT_TRUE(iter.Valid());
      EXPECT_EQ(k, iter.key());
      iter.Next();
    }
    EXPECT_FALSE(iter.Valid());
  }

  // Seek lands on lower_bound for every possible target.
  for (Key i = 0; i < static_cast<Key>(R); i++) {
    SkipList<Key, TestComparator>::Iterator iter(&list);
    iter.Seek(i);
    auto model = keys.lower_bound(i);
    for (int j = 0; j < 3; j++) {
      if (model == keys.end()) {
        ASSERT_FALSE(iter.Valid());
        break;
      }
      ASSERT_TRUE(iter.Valid());
      EXPECT_EQ(*model, iter.key());
      ++model;
      iter.Next();
    }
  }
}

// One writer inserts increasing keys while readers continuously scan and
// check that what they see is sorted, gap-free up to what was published,
// and never goes backwards. Run under TSan in CI.
TEST(SkipListTest, ConcurrentReadersWithOneWriter) {
  Arena arena;
  TestComparator cmp;
  SkipList<Key, TestComparator> list(cmp, &arena);
  constexpr Key kKeys = 20000;
  std::atomic<Key> published{0};
  std::atomic<bool> done{false};

  auto reader = [&] {
    while (!done.load(std::memory_order_acquire)) {
      const Key seen_before = published.load(std::memory_order_acquire);
      SkipList<Key, TestComparator>::Iterator iter(&list);
      iter.SeekToFirst();
      Key expected = 1;
      while (iter.Valid()) {
        ASSERT_EQ(expected, iter.key());
        expected++;
        iter.Next();
      }
      // Everything published before the scan began must have been seen.
      ASSERT_GE(expected - 1, seen_before);
    }
  };

  std::vector<std::thread> readers;
  for (int i = 0; i < 4; i++) readers.emplace_back(reader);
  for (Key k = 1; k <= kKeys; k++) {
    list.Insert(k);
    published.store(k, std::memory_order_release);
  }
  done.store(true, std::memory_order_release);
  for (auto& t : readers) t.join();
}

}  // namespace lsmkv
