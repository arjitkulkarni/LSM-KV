// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// LevelDB behind the lsmkv::DB interface, so the benchmark harness drives
// both engines through exactly the same code path. Built only with
// -DLSMKV_WITH_LEVELDB=ON.

#ifndef LSMKV_BENCH_LEVELDB_ADAPTER_H_
#define LSMKV_BENCH_LEVELDB_ADAPTER_H_

#if defined(LSMKV_WITH_LEVELDB)

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/write_batch.h"
#include "lsmkv/db.h"

namespace lsmkv::bench {

class LevelDBAdapter final : public DB {
 public:
  struct Config {
    size_t write_buffer_size = 4 << 20;
    size_t block_cache_bytes = 8 << 20;
    int bloom_bits = 10;
  };

  static Status Open(const Config& c, const std::string& dir, std::unique_ptr<DB>* out) {
    // LevelDB creates only the last path component; make the parents.
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    auto a = std::unique_ptr<LevelDBAdapter>(new LevelDBAdapter());
    a->cache_.reset(leveldb::NewLRUCache(c.block_cache_bytes));
    if (c.bloom_bits > 0) a->filter_.reset(leveldb::NewBloomFilterPolicy(c.bloom_bits));
    leveldb::Options o;
    o.create_if_missing = true;
    o.write_buffer_size = c.write_buffer_size;
    o.block_cache = a->cache_.get();
    o.filter_policy = a->filter_.get();
    o.compression = leveldb::kNoCompression;  // LSM-KV does not compress either
    o.max_open_files = 1000;
    leveldb::DB* raw = nullptr;
    leveldb::Status s = leveldb::DB::Open(o, dir, &raw);
    if (!s.ok()) return Status::IOError("leveldb open", s.ToString());
    a->db_.reset(raw);
    *out = std::move(a);
    return Status::OK();
  }

  ~LevelDBAdapter() override { db_.reset(); }

  Status Put(const WriteOptions& o, const Slice& k, const Slice& v) override {
    leveldb::WriteOptions lo;
    lo.sync = o.sync;
    return Convert(db_->Put(lo, leveldb::Slice(k.data(), k.size()),
                            leveldb::Slice(v.data(), v.size())));
  }

  Status Delete(const WriteOptions& o, const Slice& k) override {
    leveldb::WriteOptions lo;
    lo.sync = o.sync;
    return Convert(db_->Delete(lo, leveldb::Slice(k.data(), k.size())));
  }

  Status Write(const WriteOptions& o, WriteBatch* updates) override {
    struct Copier final : WriteBatch::Handler {
      leveldb::WriteBatch out;
      void Put(const Slice& k, const Slice& v) override {
        out.Put(leveldb::Slice(k.data(), k.size()), leveldb::Slice(v.data(), v.size()));
      }
      void Delete(const Slice& k) override {
        out.Delete(leveldb::Slice(k.data(), k.size()));
      }
    } copier;
    Status s = updates->Iterate(&copier);
    if (!s.ok()) return s;
    leveldb::WriteOptions lo;
    lo.sync = o.sync;
    return Convert(db_->Write(lo, &copier.out));
  }

  Status Get(const ReadOptions& o, const Slice& k, std::string* value) override {
    leveldb::ReadOptions lo;
    lo.fill_cache = o.fill_cache;
    return Convert(db_->Get(lo, leveldb::Slice(k.data(), k.size()), value));
  }

  std::unique_ptr<Iterator> NewIterator(const ReadOptions& o) override {
    leveldb::ReadOptions lo;
    lo.fill_cache = o.fill_cache;
    return std::make_unique<IteratorAdapter>(db_->NewIterator(lo));
  }

  bool GetProperty(const Slice& property, std::string* value) override {
    std::string p = property.ToString();
    if (p == "lsmkv.stats") p = "leveldb.stats";
    if (p == "lsmkv.json-stats") {
      *value = "{\"engine\":\"leveldb-1.23\"}";
      return true;
    }
    return db_->GetProperty(p, value);
  }

  Status Flush() override { return Status::OK(); }  // no public API in LevelDB

  Status WaitForCompactions() override {
    // LevelDB exposes no "compaction idle" signal; wait until its stats
    // stop changing for a full second.
    std::string prev;
    for (int i = 0; i < 600; i++) {
      std::string cur;
      db_->GetProperty("leveldb.stats", &cur);
      if (cur == prev) return Status::OK();
      prev = cur;
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return Status::OK();
  }

  Status CompactAll() override {
    db_->CompactRange(nullptr, nullptr);
    return Status::OK();
  }

 private:
  LevelDBAdapter() = default;

  static Status Convert(const leveldb::Status& s) {
    if (s.ok()) return Status::OK();
    if (s.IsNotFound()) return Status::NotFound(Slice());
    if (s.IsCorruption()) return Status::Corruption(s.ToString());
    return Status::IOError(s.ToString());
  }

  class IteratorAdapter final : public Iterator {
   public:
    explicit IteratorAdapter(leveldb::Iterator* it) : it_(it) {}
    bool Valid() const override { return it_->Valid(); }
    void SeekToFirst() override { it_->SeekToFirst(); }
    void Seek(const Slice& t) override { it_->Seek(leveldb::Slice(t.data(), t.size())); }
    void Next() override { it_->Next(); }
    Slice key() const override { return Slice(it_->key().data(), it_->key().size()); }
    Slice value() const override {
      return Slice(it_->value().data(), it_->value().size());
    }
    Status status() const override { return Convert(it_->status()); }

   private:
    std::unique_ptr<leveldb::Iterator> it_;
  };

  std::unique_ptr<leveldb::Cache> cache_;
  std::unique_ptr<const leveldb::FilterPolicy> filter_;
  std::unique_ptr<leveldb::DB> db_;
};

}  // namespace lsmkv::bench

#endif  // LSMKV_WITH_LEVELDB
#endif  // LSMKV_BENCH_LEVELDB_ADAPTER_H_
