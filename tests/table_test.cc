// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "table/table.h"

#include <gtest/gtest.h>

#include <map>

#include "db/dbformat.h"
#include "lsmkv/cache.h"
#include "lsmkv/filter_policy.h"
#include "lsmkv/metrics.h"
#include "table/table_builder.h"
#include "test_util.h"
#include "util/random.h"

namespace lsmkv {

namespace {

class TableTest : public ::testing::Test {
 protected:
  TableTest() {
    options_.comparator = BytewiseComparator();
    options_.filter_policy = NewBloomFilterPolicy(10);
    options_.block_cache = NewLRUCache(1 << 20);
    options_.block_size = 1024;
    options_.counters.bloom_checks = registry_.GetCounter("checks", "");
    options_.counters.bloom_negatives = registry_.GetCounter("negatives", "");
    options_.counters.block_reads = registry_.GetCounter("reads", "");
    options_.counters.cache_hits = registry_.GetCounter("hits", "");
  }

  void Build() {
    test::StringSink sink;
    TableBuilder builder(options_, &sink);
    for (const auto& [k, v] : model_) builder.Add(k, v);
    ASSERT_TRUE(builder.Finish().ok());
    ASSERT_EQ(sink.contents().size(), builder.FileSize());
    contents_ = sink.contents();
    Reopen();
  }

  void Reopen() {
    auto file = std::make_unique<test::StringSource>(contents_);
    const uint64_t size = file->contents().size();
    Status s = Table::Open(options_, std::move(file), size, &table_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  // Returns the value found at the first entry >= key, or "<none>".
  std::string Lookup(const std::string& key, Status* status = nullptr) {
    std::string found = "<none>";
    Status s = table_->InternalGet(ReadOptions(), key,
                                   [&](const Slice& k, const Slice& v) {
                                     if (k == Slice(key)) found = v.ToString();
                                   });
    if (status != nullptr) *status = s;
    return found;
  }

  MetricsRegistry registry_;
  TableOptions options_;
  std::map<std::string, std::string> model_;
  std::string contents_;
  std::shared_ptr<Table> table_;
};

}  // namespace

TEST_F(TableTest, EmptyTable) {
  Build();
  auto it = table_->NewIterator(ReadOptions());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
}

TEST_F(TableTest, IterateAndPointLookups) {
  Random rnd(42);
  for (int i = 0; i < 5000; i++) {
    model_[test::Key(rnd.Uniform(1000000))] = test::RandomString(&rnd, 100);
  }
  Build();

  auto it = table_->NewIterator(ReadOptions());
  it->SeekToFirst();
  for (const auto& [k, v] : model_) {
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(k, it->key().ToString());
    ASSERT_EQ(v, it->value().ToString());
    it->Next();
  }
  ASSERT_FALSE(it->Valid());

  for (const auto& [k, v] : model_) ASSERT_EQ(v, Lookup(k));

  // Seek to absent keys lands on lower_bound.
  for (int i = 0; i < 1000; i++) {
    const std::string target = test::Key(rnd.Uniform(1000000));
    it->Seek(target);
    auto m = model_.lower_bound(target);
    if (m == model_.end()) {
      ASSERT_FALSE(it->Valid());
    } else {
      ASSERT_TRUE(it->Valid());
      ASSERT_EQ(m->first, it->key().ToString());
    }
  }
}

TEST_F(TableTest, BloomFilterSkipsBlockReadsForAbsentKeys) {
  for (int i = 0; i < 10000; i += 2) model_[test::Key(i)] = "v";
  Build();
  const uint64_t reads_before =
      options_.counters.block_reads->Value() + options_.counters.cache_hits->Value();
  int false_positives = 0;
  for (int i = 1; i < 10000; i += 2) {  // every odd key is absent
    if (table_->KeyMayMatch(test::Key(i))) false_positives++;
    EXPECT_EQ("<none>", Lookup(test::Key(i)));
  }
  const uint64_t reads_after =
      options_.counters.block_reads->Value() + options_.counters.cache_hits->Value();
  // ~1% FP at 10 bits/key: at most a few dozen of 5000 lookups touch a block.
  EXPECT_LT(false_positives, 150);
  // Only a filter false positive can cost a block read.
  EXPECT_LE(reads_after - reads_before, static_cast<uint64_t>(false_positives));
  EXPECT_GT(options_.counters.bloom_negatives->Value(), 4800u);
}

TEST_F(TableTest, BlockCacheServesRepeatReads) {
  for (int i = 0; i < 2000; i++) model_[test::Key(i)] = std::string(50, 'x');
  Build();
  ASSERT_EQ(std::string(50, 'x'), Lookup(test::Key(7)));
  const uint64_t disk_reads = options_.counters.block_reads->Value();
  ASSERT_EQ(std::string(50, 'x'), Lookup(test::Key(7)));
  EXPECT_EQ(disk_reads, options_.counters.block_reads->Value());
  EXPECT_GE(options_.counters.cache_hits->Value(), 1u);
}

TEST_F(TableTest, CorruptDataBlockIsReportedNotReturned) {
  for (int i = 0; i < 2000; i++) model_[test::Key(i)] = std::string(50, 'x');
  options_.block_cache = nullptr;  // force every read to hit the "disk"
  Build();
  contents_[200] = static_cast<char>(contents_[200] ^ 0x5a);  // inside block 0
  Reopen();
  Status s;
  EXPECT_EQ("<none>", Lookup(test::Key(0), &s));
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();

  auto it = table_->NewIterator(ReadOptions());
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) n++;
  EXPECT_LT(n, 2000);  // the damaged block was skipped...
  EXPECT_TRUE(it->status().IsCorruption());  // ...and the caller is told
}

TEST_F(TableTest, TruncatedFileIsRejected) {
  model_["a"] = "b";
  Build();
  contents_.resize(contents_.size() - 3);
  auto file = std::make_unique<test::StringSource>(contents_);
  std::shared_ptr<Table> t;
  EXPECT_FALSE(Table::Open(options_, std::move(file), contents_.size(), &t).ok());
}

}  // namespace lsmkv
