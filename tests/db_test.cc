// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "lsmkv/db.h"

#include <gtest/gtest.h>

#include <map>

#include "lsmkv/metrics.h"
#include "test_util.h"
#include "util/random.h"

namespace lsmkv {

namespace {

class DBTest : public ::testing::Test {
 protected:
  DBTest() : dir_("db_test") {
    options_.write_buffer_size = 64 << 10;  // small: exercise flush/compaction
    options_.max_file_size = 64 << 10;
    options_.max_bytes_for_level_base = 256 << 10;
    Reopen();
  }

  void Reopen() {
    db_.reset();
    db_ = test::OpenOrDie(options_, dir_.path());
  }

  void Close() { db_.reset(); }

  std::string Get(const std::string& k) {
    std::string result;
    Status s = db_->Get(ReadOptions(), k, &result);
    if (s.IsNotFound()) return "NOT_FOUND";
    if (!s.ok()) return s.ToString();
    return result;
  }

  Status Put(const std::string& k, const std::string& v) {
    return db_->Put(WriteOptions(), k, v);
  }
  Status Delete(const std::string& k) { return db_->Delete(WriteOptions(), k); }

  std::string Contents() {
    std::string out;
    auto it = db_->NewIterator(ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      out += it->key().ToString() + "=" + it->value().ToString() + ";";
    }
    EXPECT_TRUE(it->status().ok());
    return out;
  }

  int TotalTableFiles() {
    int n = 0;
    for (int level = 0; level < 7; level++) {
      std::string v;
      db_->GetProperty("lsmkv.num-files-at-level" + std::to_string(level), &v);
      n += std::stoi(v);
    }
    return n;
  }

  test::TempDir dir_;
  Options options_;
  std::unique_ptr<DB> db_;
};

}  // namespace

TEST_F(DBTest, Empty) {
  EXPECT_EQ("NOT_FOUND", Get("foo"));
  EXPECT_EQ("", Contents());
}

TEST_F(DBTest, ReadWrite) {
  ASSERT_TRUE(Put("foo", "v1").ok());
  EXPECT_EQ("v1", Get("foo"));
  ASSERT_TRUE(Put("bar", "v2").ok());
  ASSERT_TRUE(Put("foo", "v3").ok());
  EXPECT_EQ("v3", Get("foo"));
  EXPECT_EQ("v2", Get("bar"));
}

TEST_F(DBTest, DeleteHidesValue) {
  ASSERT_TRUE(Put("foo", "v1").ok());
  ASSERT_TRUE(Delete("foo").ok());
  EXPECT_EQ("NOT_FOUND", Get("foo"));
  ASSERT_TRUE(Put("foo", "v2").ok());
  EXPECT_EQ("v2", Get("foo"));
}

TEST_F(DBTest, EmptyKeyAndValue) {
  ASSERT_TRUE(Put("", "empty-key").ok());
  ASSERT_TRUE(Put("k", "").ok());
  EXPECT_EQ("empty-key", Get(""));
  EXPECT_EQ("", Get("k"));
  Reopen();
  EXPECT_EQ("empty-key", Get(""));
  EXPECT_EQ("", Get("k"));
}

TEST_F(DBTest, BinaryKeysAndValues) {
  const std::string k("a\0b\xff", 4);
  const std::string v("\0\0\x01\x80", 4);
  ASSERT_TRUE(Put(k, v).ok());
  EXPECT_EQ(v, Get(k));
  ASSERT_TRUE(db_->Flush().ok());
  EXPECT_EQ(v, Get(k));
}

TEST_F(DBTest, SurvivesReopenFromWalOnly) {
  ASSERT_TRUE(Put("foo", "v1").ok());
  ASSERT_TRUE(Put("baz", "v5").ok());
  Reopen();  // no flush: everything comes back from the WAL
  EXPECT_EQ("v1", Get("foo"));
  EXPECT_EQ("v5", Get("baz"));
  ASSERT_TRUE(Put("bar", "v2").ok());
  ASSERT_TRUE(Put("foo", "v3").ok());
  Reopen();
  EXPECT_EQ("v3", Get("foo"));
  EXPECT_EQ("v2", Get("bar"));
  EXPECT_EQ("v5", Get("baz"));
}

TEST_F(DBTest, SurvivesReopenFromSSTables) {
  for (int i = 0; i < 100; i++) ASSERT_TRUE(Put(test::Key(i), "v" + std::to_string(i)).ok());
  ASSERT_TRUE(db_->Flush().ok());
  EXPECT_GE(TotalTableFiles(), 1);
  Reopen();
  for (int i = 0; i < 100; i++) EXPECT_EQ("v" + std::to_string(i), Get(test::Key(i)));
}

TEST_F(DBTest, LargeValuesSpanWalBlocks) {
  Random rnd(301);
  const std::string big1 = test::RandomString(&rnd, 100000);  // > 3 WAL blocks
  const std::string big2 = test::RandomString(&rnd, 250000);
  ASSERT_TRUE(Put("big1", big1).ok());
  ASSERT_TRUE(Put("big2", big2).ok());
  Reopen();
  EXPECT_EQ(big1, Get("big1"));
  EXPECT_EQ(big2, Get("big2"));
  ASSERT_TRUE(db_->CompactAll().ok());
  EXPECT_EQ(big1, Get("big1"));
  EXPECT_EQ(big2, Get("big2"));
}

TEST_F(DBTest, IteratorOrderSeekAndTombstones) {
  ASSERT_TRUE(Put("a", "va").ok());
  ASSERT_TRUE(Put("c", "vc").ok());
  ASSERT_TRUE(db_->Flush().ok());  // a, c in an SSTable
  ASSERT_TRUE(Put("b", "vb").ok());
  ASSERT_TRUE(Delete("c").ok());   // tombstone in the memtable shadows c
  ASSERT_TRUE(Put("d", "vd").ok());
  EXPECT_EQ("a=va;b=vb;d=vd;", Contents());

  auto it = db_->NewIterator(ReadOptions());
  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("b", it->key().ToString());
  it->Seek("c");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("d", it->key().ToString());
  it->Seek("z");
  EXPECT_FALSE(it->Valid());
}

TEST_F(DBTest, IteratorIsAPointInTimeView) {
  ASSERT_TRUE(Put("a", "1").ok());
  ASSERT_TRUE(Put("b", "1").ok());
  auto it = db_->NewIterator(ReadOptions());
  // Writes after creation are invisible to it, even after a flush and a
  // compaction rewrite the files underneath.
  ASSERT_TRUE(Put("a", "2").ok());
  ASSERT_TRUE(Put("c", "2").ok());
  ASSERT_TRUE(Delete("b").ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  std::string seen;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    seen += it->key().ToString() + "=" + it->value().ToString() + ";";
  }
  EXPECT_EQ("a=1;b=1;", seen);
  EXPECT_EQ("a=2;c=2;", Contents());
}

TEST_F(DBTest, ManyKeysThroughFlushAndCompaction) {
  Random rnd(17);
  std::map<std::string, std::string> model;
  for (int i = 0; i < 20000; i++) {
    const std::string k = test::Key(rnd.Uniform(5000));
    const std::string v = test::RandomString(&rnd, 100);
    ASSERT_TRUE(Put(k, v).ok());
    model[k] = v;
    if (i % 7 == 0) {
      const std::string d = test::Key(rnd.Uniform(5000));
      ASSERT_TRUE(Delete(d).ok());
      model.erase(d);
    }
  }
  ASSERT_TRUE(db_->WaitForCompactions().ok());
  std::string stats;
  ASSERT_TRUE(db_->GetProperty("lsmkv.stats", &stats));
  EXPECT_NE(std::string::npos, stats.find("Write amplification"));
  for (const auto& [k, v] : model) ASSERT_EQ(v, Get(k)) << k;
  Reopen();
  std::string expected;
  for (const auto& [k, v] : model) expected += k + "=" + v + ";";
  EXPECT_EQ(expected, Contents());
}

TEST_F(DBTest, CompactAllDropsTombstonesAtTheBottom) {
  for (int i = 0; i < 2000; i++) ASSERT_TRUE(Put(test::Key(i), std::string(100, 'x')).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  for (int i = 0; i < 2000; i++) ASSERT_TRUE(Delete(test::Key(i)).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  EXPECT_EQ("", Contents());
  // Every value and every tombstone is gone: nothing left on disk.
  EXPECT_EQ(0, TotalTableFiles());
}

TEST_F(DBTest, WriteBatchIsAtomicAcrossRecovery) {
  WriteBatch batch;
  batch.Put("x", "1");
  batch.Put("y", "2");
  batch.Delete("x");
  ASSERT_TRUE(db_->Write(WriteOptions(), &batch).ok());
  Reopen();
  EXPECT_EQ("NOT_FOUND", Get("x"));
  EXPECT_EQ("2", Get("y"));
}

TEST_F(DBTest, SyncWrites) {
  WriteOptions sync;
  sync.sync = true;
  for (int i = 0; i < 50; i++) ASSERT_TRUE(db_->Put(sync, test::Key(i), "v").ok());
  Reopen();
  for (int i = 0; i < 50; i++) EXPECT_EQ("v", Get(test::Key(i)));
}

TEST_F(DBTest, SecondOpenOfSameDirectoryFails) {
  std::unique_ptr<DB> second;
  Status s = DB::Open(options_, dir_.path(), &second);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(nullptr, second);
}

TEST_F(DBTest, CreateIfMissingAndErrorIfExists) {
  test::TempDir fresh("db_test_fresh");
  Options o;
  o.create_if_missing = false;
  std::unique_ptr<DB> db;
  EXPECT_TRUE(DB::Open(o, fresh.path() + "/nope", &db).IsInvalidArgument());
  Close();
  o.create_if_missing = true;
  o.error_if_exists = true;
  EXPECT_TRUE(DB::Open(o, dir_.path(), &db).IsInvalidArgument());
}

TEST_F(DBTest, Properties) {
  ASSERT_TRUE(Put("k", "v").ok());
  std::string v;
  EXPECT_TRUE(db_->GetProperty("lsmkv.num-files-at-level0", &v));
  EXPECT_TRUE(db_->GetProperty("lsmkv.write-amplification", &v));
  EXPECT_TRUE(db_->GetProperty("lsmkv.json-stats", &v));
  EXPECT_EQ('{', v.front());
  EXPECT_TRUE(db_->GetProperty("lsmkv.prometheus", &v));
  EXPECT_NE(std::string::npos, v.find("lsmkv_ops_total{op=\"put\"} 1"));
  EXPECT_FALSE(db_->GetProperty("lsmkv.nonsense", &v));
  EXPECT_FALSE(db_->GetProperty("other.stats", &v));
}

TEST_F(DBTest, MetricsTrackTheWorkload) {
  auto registry = std::make_shared<MetricsRegistry>();
  options_.metrics = registry;
  Reopen();
  for (int i = 0; i < 1000; i++) ASSERT_TRUE(Put(test::Key(i), "v").ok());
  for (int i = 0; i < 500; i++) Get(test::Key(i));
  Get("missing");
  ASSERT_TRUE(db_->Flush().ok());
  double v = 0;
  ASSERT_TRUE(registry->GetValue("lsmkv_ops_total", {{"op", "put"}}, &v));
  EXPECT_EQ(1000, v);
  ASSERT_TRUE(registry->GetValue("lsmkv_ops_total", {{"op", "get"}}, &v));
  EXPECT_EQ(501, v);
  ASSERT_TRUE(registry->GetValue("lsmkv_get_misses_total", {}, &v));
  EXPECT_EQ(1, v);
  ASSERT_TRUE(registry->GetValue("lsmkv_flushes_total", {}, &v));
  EXPECT_GE(v, 1);
}

TEST_F(DBTest, DestroyDBRemovesEverything) {
  ASSERT_TRUE(Put("k", "v").ok());
  Close();
  ASSERT_TRUE(DestroyDB(dir_.path(), options_).ok());
  Options o;
  o.create_if_missing = false;
  std::unique_ptr<DB> db;
  EXPECT_FALSE(DB::Open(o, dir_.path(), &db).ok());
}

TEST_F(DBTest, ShardCountDoesNotChangeResults) {
  for (int shards : {1, 3, 8, 16}) {
    test::TempDir d("db_shards");
    Options o = options_;
    o.memtable_shards = shards;
    auto db = test::OpenOrDie(o, d.path());
    for (int i = 0; i < 3000; i++) {
      ASSERT_TRUE(db->Put(WriteOptions(), test::Key(i % 1000), std::to_string(i)).ok());
    }
    int n = 0;
    auto it = db->NewIterator(ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      EXPECT_EQ(std::to_string(n + 2000), it->value().ToString()) << shards;
      n++;
    }
    EXPECT_EQ(1000, n);
  }
}

}  // namespace lsmkv
