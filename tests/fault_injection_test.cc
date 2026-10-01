// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Power loss and fsync failure, simulated with FaultInjectionEnv.

#include "util/fault_injection_env.h"

#include <gtest/gtest.h>

#include <map>

#include "lsmkv/db.h"
#include "test_util.h"
#include "util/random.h"

namespace lsmkv {

namespace {

class FaultInjectionTest : public ::testing::Test {
 protected:
  FaultInjectionTest() : dir_("fault_test"), env_(Env::Default()) {
    options_.env = &env_;
    options_.write_buffer_size = 64 << 10;  // force flushes + compactions
    options_.max_file_size = 64 << 10;
    options_.max_bytes_for_level_base = 256 << 10;
  }

  Status Open() {
    db_.reset();
    return DB::Open(options_, dir_.path(), &db_);
  }

  test::TempDir dir_;
  FaultInjectionEnv env_;
  Options options_;
  std::unique_ptr<DB> db_;
};

}  // namespace

// Writes acknowledged with sync=true must survive losing every byte that
// was never fsync'd -- in the WAL, the SSTables, the MANIFEST and CURRENT.
// Repeated across rounds so the loss lands in the middle of flushes and
// compactions too.
TEST_F(FaultInjectionTest, SyncedWritesSurvivePowerLoss) {
  Random rnd(2024);
  std::map<std::string, std::string> synced;  // acked with sync=true
  for (int round = 0; round < 12; round++) {
    ASSERT_TRUE(Open().ok());
    WriteOptions sync_opt;
    sync_opt.sync = true;
    for (int i = 0; i < 300; i++) {
      const std::string k = test::Key(rnd.Uniform(2000));
      const std::string v = test::RandomString(&rnd, 200);
      ASSERT_TRUE(db_->Put(sync_opt, k, v).ok());
      synced[k] = v;
    }
    // Un-synced writes: allowed to vanish, but must not corrupt anything.
    for (int i = 0; i < 300; i++) {
      const std::string k = test::Key(2000 + rnd.Uniform(2000));
      ASSERT_TRUE(db_->Put(WriteOptions(), k, "unsynced").ok());
    }
    db_.reset();
    ASSERT_TRUE(env_.DropUnsyncedData().ok());  // the power goes out
    env_.ResetState();

    Status s = Open();
    ASSERT_TRUE(s.ok()) << "round " << round << ": " << s.ToString();
    for (const auto& [k, v] : synced) {
      std::string got;
      Status g = db_->Get(ReadOptions(), k, &got);
      ASSERT_TRUE(g.ok()) << "round " << round << " lost synced key " << k << ": "
                          << g.ToString();
      ASSERT_EQ(v, got);
    }
    // The whole DB is still readable end to end.
    auto it = db_->NewIterator(ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
    }
    ASSERT_TRUE(it->status().ok()) << it->status().ToString();
    db_.reset();
  }
}

// After a failed fsync the engine must stop acknowledging writes: it cannot
// know what reached the disk, and retrying fsync may "succeed" after the
// kernel has already dropped the dirty pages.
TEST_F(FaultInjectionTest, FailedFsyncMakesTheDBReadOnly) {
  ASSERT_TRUE(Open().ok());
  WriteOptions sync_opt;
  sync_opt.sync = true;
  for (int i = 0; i < 50; i++) {
    ASSERT_TRUE(db_->Put(sync_opt, test::Key(i), "acked").ok());
  }
  env_.FailSyncAfter(env_.sync_calls() + 1);  // the very next fsync fails

  Status s = db_->Put(sync_opt, "doomed", "x");
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  // Every later write is refused, synced or not...
  EXPECT_FALSE(db_->Put(WriteOptions(), "later", "y").ok());
  EXPECT_FALSE(db_->Put(sync_opt, "later2", "y").ok());
  // ...but reads keep working (degraded, not down).
  std::string v;
  ASSERT_TRUE(db_->Get(ReadOptions(), test::Key(7), &v).ok());
  EXPECT_EQ("acked", v);

  // Crash, lose un-synced data, restart with a healthy disk: all acked
  // writes are back and the DB accepts writes again.
  db_.reset();
  ASSERT_TRUE(env_.DropUnsyncedData().ok());
  env_.ResetState();
  ASSERT_TRUE(Open().ok());
  for (int i = 0; i < 50; i++) {
    ASSERT_TRUE(db_->Get(ReadOptions(), test::Key(i), &v).ok()) << i;
    EXPECT_EQ("acked", v);
  }
  EXPECT_TRUE(db_->Put(sync_opt, "after-restart", "z").ok());
}

TEST_F(FaultInjectionTest, FilesystemGoingAwayFailsWritesCleanly) {
  ASSERT_TRUE(Open().ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "before", "1").ok());
  env_.SetFilesystemActive(false);
  EXPECT_FALSE(db_->Put(WriteOptions(), "during", "2").ok());
  db_.reset();
  env_.ResetState();
  ASSERT_TRUE(Open().ok());
  std::string v;
  ASSERT_TRUE(db_->Get(ReadOptions(), "before", &v).ok());
  EXPECT_TRUE(db_->Get(ReadOptions(), "during", &v).IsNotFound());
}

}  // namespace lsmkv
