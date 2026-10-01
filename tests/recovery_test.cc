// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// The availability/reliability story, as executable assertions: what Open()
// does with every kind of damage a crash or a bad disk can leave behind.

#include <gtest/gtest.h>

#include <cstdio>

#include "lsmkv/db.h"
#include "test_util.h"
#include "wal/log_format.h"

namespace lsmkv {

namespace {

class RecoveryTest : public ::testing::Test {
 protected:
  RecoveryTest() : dir_("recovery_test") {}

  Status Open(WalRecoveryMode mode = WalRecoveryMode::kTolerateCorruptedTail) {
    db_.reset();
    Options o;
    o.wal_recovery_mode = mode;
    return DB::Open(o, dir_.path(), &db_);
  }

  void OpenOrFail(WalRecoveryMode mode = WalRecoveryMode::kTolerateCorruptedTail) {
    Status s = Open(mode);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  void Close() { db_.reset(); }

  // Writes n records "key<i>" -> value(i) with the given value size.
  void Fill(int from, int to, size_t value_size = 100) {
    for (int i = from; i < to; i++) {
      ASSERT_TRUE(db_->Put(WriteOptions(), test::Key(i), Value(i, value_size)).ok());
    }
  }

  static std::string Value(int i, size_t size) {
    std::string v = std::to_string(i) + ":";
    v.resize(size, static_cast<char>('a' + i % 26));
    return v;
  }

  // Number of keys in [from, to) readable with the right value; fails the
  // test if any key returns a *wrong* value (that would be corruption).
  int CountIntact(int from, int to, size_t value_size = 100) {
    int n = 0;
    for (int i = from; i < to; i++) {
      std::string v;
      Status s = db_->Get(ReadOptions(), test::Key(i), &v);
      if (s.ok()) {
        EXPECT_EQ(Value(i, value_size), v) << "key " << i;
        n++;
      } else {
        EXPECT_TRUE(s.IsNotFound()) << s.ToString();
      }
    }
    return n;
  }

  std::string NewestLog() {
    auto logs = test::ListFiles(dir_.path(), ".log");
    EXPECT_FALSE(logs.empty());
    return test::FileNameFor(dir_.path(), logs.back(), ".log");
  }

  test::TempDir dir_;
  std::unique_ptr<DB> db_;
};

}  // namespace

TEST_F(RecoveryTest, CleanReopenKeepsEverything) {
  OpenOrFail();
  Fill(0, 1000);
  Close();
  OpenOrFail();
  EXPECT_EQ(1000, CountIntact(0, 1000));
}

TEST_F(RecoveryTest, TornTailFromTruncationIsTolerated) {
  OpenOrFail();
  Fill(0, 100);
  Close();
  // Chop the last record in half: the write that was in flight at the crash.
  test::TruncateBy(NewestLog(), 60);
  OpenOrFail();
  EXPECT_EQ(99, CountIntact(0, 100));  // every complete record survives
}

TEST_F(RecoveryTest, TailIsTruncatedSoLaterAppendsStayReadable) {
  OpenOrFail();
  Fill(0, 100);
  Close();
  test::TruncateBy(NewestLog(), 60);
  OpenOrFail();
  // Without truncating the torn bytes, these appends would land *after*
  // garbage and the next Open would see "damage followed by valid data".
  Fill(100, 200);
  Close();
  OpenOrFail();
  EXPECT_EQ(199, CountIntact(0, 200));
}

TEST_F(RecoveryTest, TornFragmentedRecordIsTolerated) {
  OpenOrFail();
  Fill(0, 10);
  ASSERT_TRUE(db_->Put(WriteOptions(), "big", std::string(100000, 'B')).ok());
  Close();
  test::TruncateBy(NewestLog(), 30000);  // lose the tail fragments
  OpenOrFail();
  EXPECT_EQ(10, CountIntact(0, 10));
  std::string v;
  EXPECT_TRUE(db_->Get(ReadOptions(), "big", &v).IsNotFound());
}

TEST_F(RecoveryTest, ZeroFilledTailIsTolerated) {
  OpenOrFail();
  Fill(0, 50);
  Close();
  test::AppendBytes(NewestLog(), std::string(3000, '\0'));  // preallocated junk
  OpenOrFail();
  EXPECT_EQ(50, CountIntact(0, 50));
}

TEST_F(RecoveryTest, GarbageTailIsTolerated) {
  OpenOrFail();
  Fill(0, 50);
  Close();
  test::AppendBytes(NewestLog(), "\x12\x34\x56\x78\x10\x00\x01garbage-bytes");
  OpenOrFail();
  EXPECT_EQ(50, CountIntact(0, 50));
}

TEST_F(RecoveryTest, CorruptionFollowedByValidRecordsRefusesToOpen) {
  OpenOrFail();
  // Large values: record 0 lives in block 0 and later records in later
  // blocks, so the reader can resynchronize past the damage.
  Fill(0, 20, 10000);
  Close();
  test::CorruptByte(NewestLog(), 100);  // inside the first record's payload
  // Default policy: valid data after damage means acknowledged writes would
  // be silently dropped. Refuse to open.
  Status s = Open();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(RecoveryTest, PointInTimeRecoveryOpensWithTheValidPrefix) {
  OpenOrFail();
  Fill(0, 20, 10000);
  Close();
  // Damage the fourth record (offset ~3 * 10 KB), well inside block 0.
  test::CorruptByte(NewestLog(), 3 * 10030 + 500);
  OpenOrFail(WalRecoveryMode::kPointInTimeRecovery);
  const int intact = CountIntact(0, 20, 10000);
  EXPECT_GE(intact, 1);
  EXPECT_LT(intact, 20);
  // Consistent prefix: whatever survived is exactly keys [0, intact).
  EXPECT_EQ(intact, CountIntact(0, intact, 10000));
  // And the DB stays writable and re-openable afterwards.
  Fill(100, 110, 10000);
  Close();
  OpenOrFail(WalRecoveryMode::kTolerateCorruptedTail);
  EXPECT_EQ(10, CountIntact(100, 110, 10000));
}

TEST_F(RecoveryTest, AbsoluteConsistencyRejectsEvenATornTail) {
  OpenOrFail();
  Fill(0, 10);
  Close();
  test::TruncateBy(NewestLog(), 10);
  Status s = Open(WalRecoveryMode::kAbsoluteConsistency);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  OpenOrFail();  // the default policy accepts it
  EXPECT_EQ(9, CountIntact(0, 10));
}

TEST_F(RecoveryTest, CorruptSSTableBlockIsAnErrorNotWrongData) {
  OpenOrFail();
  Fill(0, 2000);
  ASSERT_TRUE(db_->CompactAll().ok());
  Close();
  auto tables = test::ListFiles(dir_.path(), ".sst");
  ASSERT_FALSE(tables.empty());
  test::CorruptByte(test::FileNameFor(dir_.path(), tables[0], ".sst"), 50);
  OpenOrFail();
  int errors = 0;
  for (int i = 0; i < 2000; i++) {
    std::string v;
    Status s = db_->Get(ReadOptions(), test::Key(i), &v);
    if (s.IsCorruption()) {
      errors++;
    } else {
      ASSERT_TRUE(s.ok()) << s.ToString();
      ASSERT_EQ(Value(i, 100), v);  // never a wrong value
    }
  }
  EXPECT_GT(errors, 0);
}

TEST_F(RecoveryTest, MissingSSTableFailsLoudly) {
  OpenOrFail();
  Fill(0, 500);
  ASSERT_TRUE(db_->CompactAll().ok());
  Close();
  auto tables = test::ListFiles(dir_.path(), ".sst");
  ASSERT_FALSE(tables.empty());
  std::remove(test::FileNameFor(dir_.path(), tables[0], ".sst").c_str());
  Status s = Open();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(RecoveryTest, TornManifestTailIsIgnored) {
  OpenOrFail();
  Fill(0, 500);
  ASSERT_TRUE(db_->Flush().ok());
  Close();
  // Find the live MANIFEST via CURRENT and append a torn edit to it.
  std::string current;
  ASSERT_TRUE(ReadFileToString(Env::Default(), dir_.path() + "/CURRENT", &current).ok());
  current.pop_back();
  test::AppendBytes(dir_.path() + "/" + current, std::string("\x01\x02\x03\x04\x40\x00", 6));
  OpenOrFail();
  EXPECT_EQ(500, CountIntact(0, 500));
}

TEST_F(RecoveryTest, RepeatedCrashRecoveryIsIdempotent) {
  OpenOrFail();
  for (int round = 0; round < 5; round++) {
    Fill(round * 100, round * 100 + 100);
    Close();
    test::TruncateBy(NewestLog(), 7);  // tear the final record every time
    OpenOrFail();
  }
  // Each round lost exactly its final (torn) record.
  EXPECT_EQ(5 * 99, CountIntact(0, 500));
}

}  // namespace lsmkv
