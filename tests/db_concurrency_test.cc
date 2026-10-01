// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Concurrent writers, readers and scanners against one DB while flushes and
// compactions run underneath. This is the test ThreadSanitizer must keep
// clean (see .github/workflows/ci.yml).

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "lsmkv/db.h"
#include "lsmkv/metrics.h"
#include "test_util.h"
#include "util/random.h"

namespace lsmkv {

namespace {

// Values are self-describing so a reader can validate anything it sees:
// "<thread>.<key>.<counter>|<padding>"
std::string MakeValue(int thread, uint64_t key, uint64_t counter) {
  std::string v = std::to_string(thread) + "." + std::to_string(key) + "." +
                  std::to_string(counter) + "|";
  v.resize(64, 'p');
  return v;
}

bool ParseValue(const std::string& v, int* thread, uint64_t* key,
                uint64_t* counter) {
  unsigned long long k = 0;
  unsigned long long c = 0;
  if (std::sscanf(v.c_str(), "%d.%llu.%llu|", thread, &k, &c) != 3) return false;
  *key = k;
  *counter = c;
  return true;
}

class DBConcurrencyTest : public ::testing::TestWithParam<int> {};

}  // namespace

TEST_P(DBConcurrencyTest, WritersReadersAndScannersAgree) {
  test::TempDir dir("db_concurrency");
  Options options;
  options.memtable_shards = GetParam();
  options.write_buffer_size = 128 << 10;
  options.max_file_size = 128 << 10;
  options.max_bytes_for_level_base = 512 << 10;
  auto db = test::OpenOrDie(options, dir.path());

  constexpr int kWriters = 6;
  constexpr int kReaders = 3;
  constexpr uint64_t kKeysPerWriter = 400;
  constexpr uint64_t kOpsPerWriter = 6000;
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  // Last value each writer wrote for each of its keys (written only by the
  // owning writer; read after join).
  std::vector<std::vector<uint64_t>> last(kWriters,
                                          std::vector<uint64_t>(kKeysPerWriter, 0));

  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; w++) {
    threads.emplace_back([&, w] {
      Random rnd(static_cast<uint64_t>(w) + 1);
      WriteOptions opt;
      for (uint64_t c = 1; c <= kOpsPerWriter; c++) {
        const uint64_t k = rnd.Uniform(kKeysPerWriter);
        const std::string key = test::Key(static_cast<uint64_t>(w) * 1000000 + k);
        opt.sync = (c % 500 == 0);  // occasional synced writes join groups
        if (!db->Put(opt, key, MakeValue(w, k, c)).ok()) errors++;
        last[static_cast<size_t>(w)][k] = c;
      }
    });
  }
  for (int r = 0; r < kReaders; r++) {
    threads.emplace_back([&, r] {
      Random rnd(1000 + static_cast<uint64_t>(r));
      while (!stop.load()) {
        const int w = static_cast<int>(rnd.Uniform(kWriters));
        const uint64_t k = rnd.Uniform(kKeysPerWriter);
        std::string v;
        Status s = db->Get(ReadOptions(),
                           test::Key(static_cast<uint64_t>(w) * 1000000 + k), &v);
        if (s.ok()) {
          int tw;
          uint64_t tk;
          uint64_t tc;
          if (!ParseValue(v, &tw, &tk, &tc) || tw != w || tk != k) errors++;
        } else if (!s.IsNotFound()) {
          errors++;
        }
      }
    });
  }
  threads.emplace_back([&] {  // scanner: sorted, well-formed, no duplicates
    while (!stop.load()) {
      auto it = db->NewIterator(ReadOptions());
      std::string prev;
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const std::string k = it->key().ToString();
        if (!prev.empty() && !(prev < k)) errors++;
        int tw;
        uint64_t tk;
        uint64_t tc;
        if (!ParseValue(it->value().ToString(), &tw, &tk, &tc)) errors++;
        prev = k;
      }
      if (!it->status().ok()) errors++;
    }
  });

  for (int w = 0; w < kWriters; w++) threads[static_cast<size_t>(w)].join();
  stop.store(true);
  for (size_t i = kWriters; i < threads.size(); i++) threads[i].join();
  ASSERT_EQ(0, errors.load());

  // Every writer's final value for every key it touched is the one we read.
  ASSERT_TRUE(db->WaitForCompactions().ok());
  for (int w = 0; w < kWriters; w++) {
    for (uint64_t k = 0; k < kKeysPerWriter; k++) {
      const uint64_t c = last[static_cast<size_t>(w)][k];
      if (c == 0) continue;
      std::string v;
      ASSERT_TRUE(db->Get(ReadOptions(),
                          test::Key(static_cast<uint64_t>(w) * 1000000 + k), &v)
                      .ok());
      EXPECT_EQ(MakeValue(w, k, c), v);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Shards, DBConcurrencyTest, ::testing::Values(1, 8));

TEST(GroupCommitTest, ConcurrentSyncWritersShareFsyncs) {
  test::TempDir dir("group_commit");
  auto registry = std::make_shared<MetricsRegistry>();
  Options options;
  options.metrics = registry;
  auto db = test::OpenOrDie(options, dir.path());

  constexpr int kThreads = 16;
  constexpr int kPerThread = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&db, t] {
      WriteOptions sync;
      sync.sync = true;
      for (int i = 0; i < kPerThread; i++) {
        ASSERT_TRUE(db->Put(sync, test::Key(static_cast<uint64_t>(t) * 100000 + i), "v").ok());
      }
    });
  }
  for (auto& th : threads) th.join();

  double groups = 0;
  double writers = 0;
  double syncs = 0;
  ASSERT_TRUE(registry->GetValue("lsmkv_wal_group_commits_total", {}, &groups));
  ASSERT_TRUE(registry->GetValue("lsmkv_wal_group_commit_writers_total", {}, &writers));
  ASSERT_TRUE(registry->GetValue("lsmkv_wal_syncs_total", {}, &syncs));
  EXPECT_EQ(kThreads * kPerThread, writers);
  // Group commit: fewer fsyncs than acknowledged synced writes.
  EXPECT_LT(syncs, writers);
  EXPECT_EQ(groups, syncs);
}

}  // namespace lsmkv
