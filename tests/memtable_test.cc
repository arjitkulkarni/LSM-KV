// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Differential test: one million random Put / Get / Delete / Scan operations
// against the memtable and against std::map as an oracle, asserting
// identical results after every read.

#include "memtable/memtable.h"

#include <gtest/gtest.h>

#include <map>
#include <thread>

#include "db/dbformat.h"
#include "test_util.h"
#include "util/random.h"

namespace lsmkv {

namespace {

// Collapses the memtable's internal-key stream into the user view, the
// same way DBIter does: first entry per user key wins, tombstones hide.
class UserView {
 public:
  explicit UserView(std::unique_ptr<Iterator> it) : it_(std::move(it)) {}

  // Returns up to `n` live (key, value) pairs starting at the first user
  // key >= start.
  std::vector<std::pair<std::string, std::string>> Scan(const std::string& start,
                                                        size_t n) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string ikey;
    AppendInternalKey(&ikey, ParsedInternalKey(start, kMaxSequenceNumber,
                                               kValueTypeForSeek));
    it_->Seek(ikey);
    std::string last_user_key;
    bool have_last = false;
    while (it_->Valid() && out.size() < n) {
      ParsedInternalKey parsed;
      EXPECT_TRUE(ParseInternalKey(it_->key(), &parsed));
      const std::string user_key = parsed.user_key.ToString();
      if (!have_last || user_key != last_user_key) {
        last_user_key = user_key;
        have_last = true;
        if (parsed.type == kTypeValue) {
          out.emplace_back(user_key, it_->value().ToString());
        }
      }
      it_->Next();
    }
    return out;
  }

 private:
  std::unique_ptr<Iterator> it_;
};

class MemTableDifferentialTest : public ::testing::TestWithParam<int> {};

}  // namespace

TEST_P(MemTableDifferentialTest, MillionRandomOpsMatchStdMap) {
  const int shards = GetParam();
  InternalKeyComparator icmp(BytewiseComparator());
  auto mem = std::make_shared<MemTable>(icmp, shards);
  std::map<std::string, std::string> oracle;
  Random rnd(0xC0FFEE + static_cast<uint64_t>(shards));
  SequenceNumber seq = 1;

  constexpr int kOps = 1000000;
  constexpr uint64_t kKeySpace = 20000;  // small => lots of overwrites
  int gets = 0;
  int scans = 0;
  for (int i = 0; i < kOps; i++) {
    const std::string key = test::Key(rnd.Uniform(kKeySpace));
    const uint64_t dice = rnd.Uniform(100);
    if (dice < 40) {  // Put
      const std::string value = test::RandomString(&rnd, rnd.Uniform(24));
      mem->Add(seq++, kTypeValue, key, value);
      oracle[key] = value;
    } else if (dice < 55) {  // Delete
      mem->Add(seq++, kTypeDeletion, key, Slice());
      oracle.erase(key);
    } else if (dice < 99) {  // Get
      gets++;
      std::string value;
      Status s;
      LookupKey lkey(key, kMaxSequenceNumber);
      const bool found = mem->Get(lkey, &value, &s);
      auto it = oracle.find(key);
      if (it == oracle.end()) {
        // Either never written (not found) or deleted (found + NotFound).
        ASSERT_TRUE(!found || s.IsNotFound()) << key;
      } else {
        ASSERT_TRUE(found) << key;
        ASSERT_TRUE(s.ok()) << key;
        ASSERT_EQ(it->second, value) << key;
      }
    } else {  // Scan of up to 20 entries
      scans++;
      UserView view(mem->NewIterator());
      const auto got = view.Scan(key, 20);
      auto it = oracle.lower_bound(key);
      for (const auto& [k, v] : got) {
        ASSERT_NE(it, oracle.end());
        ASSERT_EQ(it->first, k);
        ASSERT_EQ(it->second, v);
        ++it;
      }
      if (got.size() < 20) {
        ASSERT_EQ(it, oracle.end());
      }
    }
  }

  // Full scan equals the oracle exactly.
  UserView view(mem->NewIterator());
  const auto all = view.Scan("", oracle.size() + 10);
  ASSERT_EQ(oracle.size(), all.size());
  size_t i = 0;
  for (const auto& [k, v] : oracle) {
    ASSERT_EQ(k, all[i].first);
    ASSERT_EQ(v, all[i].second);
    i++;
  }
  EXPECT_GT(gets, 400000);
  EXPECT_GT(scans, 5000);
  EXPECT_EQ(static_cast<uint64_t>(seq - 1), mem->NumEntries());
}

INSTANTIATE_TEST_SUITE_P(Shards, MemTableDifferentialTest,
                         ::testing::Values(1, 8));

TEST(MemTableTest, NewestVersionWins) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto mem = std::make_shared<MemTable>(icmp, 4);
  // Inserted out of sequence order on purpose: after group commit, writers
  // insert in parallel, so arrival order is arbitrary.
  mem->Add(3, kTypeValue, "k", "v3");
  mem->Add(1, kTypeValue, "k", "v1");
  mem->Add(2, kTypeDeletion, "k", Slice());
  std::string value;
  Status s;
  ASSERT_TRUE(mem->Get(LookupKey("k", kMaxSequenceNumber), &value, &s));
  EXPECT_TRUE(s.ok());
  EXPECT_EQ("v3", value);
  // As of sequence 2, the key is deleted.
  ASSERT_TRUE(mem->Get(LookupKey("k", 2), &value, &s));
  EXPECT_TRUE(s.IsNotFound());
}

TEST(MemTableTest, ConcurrentWritersAcrossShards) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto mem = std::make_shared<MemTable>(icmp, 8);
  constexpr int kThreads = 8;
  constexpr int kPerThread = 20000;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&mem, t] {
      for (int i = 0; i < kPerThread; i++) {
        const uint64_t seq = static_cast<uint64_t>(t) * kPerThread + i + 1;
        mem->Add(seq, kTypeValue, test::Key(seq), std::to_string(seq));
      }
    });
  }
  // A concurrent reader must never see a torn entry.
  std::thread reader([&mem] {
    for (int i = 0; i < 50; i++) {
      auto it = mem->NewIterator();
      std::string prev;
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const std::string k = ExtractUserKey(it->key()).ToString();
        ASSERT_LT(prev, k);
        prev = k;
      }
    }
  });
  for (auto& t : threads) t.join();
  reader.join();
  EXPECT_EQ(static_cast<uint64_t>(kThreads * kPerThread), mem->NumEntries());
  for (uint64_t seq = 1; seq <= kThreads * kPerThread; seq += 997) {
    std::string value;
    Status s;
    ASSERT_TRUE(mem->Get(LookupKey(test::Key(seq), kMaxSequenceNumber), &value, &s));
    EXPECT_EQ(std::to_string(seq), value);
  }
}

}  // namespace lsmkv
