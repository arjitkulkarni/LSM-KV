// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// End-to-end differential test: random operations against the DB and
// std::map, with reopens, flushes and full compactions interleaved, so
// every read path (memtable, immutable memtable, L0, deeper levels) and the
// recovery path are compared against the oracle.

#include <gtest/gtest.h>

#include <map>

#include "lsmkv/db.h"
#include "test_util.h"
#include "util/random.h"

namespace lsmkv {

TEST(DBDifferentialTest, RandomOpsWithReopensMatchStdMap) {
  test::TempDir dir("db_differential");
  Options options;
  options.write_buffer_size = 64 << 10;
  options.max_file_size = 64 << 10;
  options.max_bytes_for_level_base = 256 << 10;
  auto db = test::OpenOrDie(options, dir.path());

  std::map<std::string, std::string> oracle;
  Random rnd(12345);
  constexpr int kOps = 150000;
  for (int i = 0; i < kOps; i++) {
    const std::string key = test::Key(rnd.Uniform(8000));
    const uint64_t dice = rnd.Uniform(1000);
    if (dice < 450) {
      const std::string v = test::RandomString(&rnd, 10 + rnd.Uniform(200));
      ASSERT_TRUE(db->Put(WriteOptions(), key, v).ok());
      oracle[key] = v;
    } else if (dice < 600) {
      ASSERT_TRUE(db->Delete(WriteOptions(), key).ok());
      oracle.erase(key);
    } else if (dice < 610) {
      WriteBatch b;
      for (int j = 0; j < 10; j++) {
        const std::string k = test::Key(rnd.Uniform(8000));
        if (rnd.OneIn(3)) {
          b.Delete(k);
          oracle.erase(k);
        } else {
          const std::string v = test::RandomString(&rnd, 50);
          b.Put(k, v);
          oracle[k] = v;
        }
      }
      ASSERT_TRUE(db->Write(WriteOptions(), &b).ok());
    } else if (dice < 990) {
      std::string v;
      Status s = db->Get(ReadOptions(), key, &v);
      auto it = oracle.find(key);
      if (it == oracle.end()) {
        ASSERT_TRUE(s.IsNotFound()) << key << " " << s.ToString();
      } else {
        ASSERT_TRUE(s.ok()) << key << " " << s.ToString();
        ASSERT_EQ(it->second, v) << key;
      }
    } else if (dice < 997) {
      // Range scan of up to 50 entries.
      auto it = db->NewIterator(ReadOptions());
      it->Seek(key);
      auto m = oracle.lower_bound(key);
      for (int j = 0; j < 50 && m != oracle.end(); j++, ++m) {
        ASSERT_TRUE(it->Valid()) << key;
        ASSERT_EQ(m->first, it->key().ToString());
        ASSERT_EQ(m->second, it->value().ToString());
        it->Next();
      }
      ASSERT_TRUE(it->status().ok());
    } else if (dice < 999) {
      db.reset();  // clean close + recovery
      db = test::OpenOrDie(options, dir.path());
      ASSERT_NE(nullptr, db);
    } else {
      ASSERT_TRUE(db->CompactAll().ok());
    }
  }

  // Final full comparison.
  auto it = db->NewIterator(ReadOptions());
  it->SeekToFirst();
  for (const auto& [k, v] : oracle) {
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(k, it->key().ToString());
    ASSERT_EQ(v, it->value().ToString());
    it->Next();
  }
  ASSERT_FALSE(it->Valid());
}

}  // namespace lsmkv
