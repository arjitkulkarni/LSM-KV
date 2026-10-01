// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// DB: the public contract of the storage engine.

#ifndef LSMKV_INCLUDE_LSMKV_DB_H_
#define LSMKV_INCLUDE_LSMKV_DB_H_

#include <memory>
#include <string>

#include "lsmkv/iterator.h"
#include "lsmkv/options.h"
#include "lsmkv/slice.h"
#include "lsmkv/status.h"
#include "lsmkv/write_batch.h"

namespace lsmkv {

// Abstract base class. Callers program against DB and never see DBImpl, so
// the benchmark harness drives LSM-KV and LevelDB through this exact
// interface (bench/leveldb_adapter.h) and the numbers are comparable.
//
// Thread safety: every method may be called concurrently from any number of
// threads without external synchronization.
//
// Ownership: Open() hands back a std::unique_ptr<DB>; iterators come back as
// std::unique_ptr<Iterator>. No call site ever writes new or delete.
class DB {
 public:
  // Opens (and if needed recovers) the database stored in directory `name`.
  static Status Open(const Options& options, const std::string& name,
                     std::unique_ptr<DB>* dbptr);

  DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  DB(DB&&) = delete;
  DB& operator=(DB&&) = delete;
  // Waits for in-flight background work, then releases every resource.
  // Does not flush the memtable: the WAL already holds its contents.
  virtual ~DB();

  virtual Status Put(const WriteOptions& options, const Slice& key,
                     const Slice& value);
  virtual Status Delete(const WriteOptions& options, const Slice& key);
  // Applies `updates` atomically with respect to crashes: after recovery
  // either every record in the batch is present or none is.
  virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;

  // On success stores the value in *value. Returns NotFound if the key is
  // absent or deleted.
  virtual Status Get(const ReadOptions& options, const Slice& key,
                     std::string* value) = 0;

  // A forward iterator over a point-in-time view: it sees every write acked
  // before it was created and none acked after. It stays valid across
  // flushes and compactions (it pins what it reads), but must be destroyed
  // before the DB.
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;

  // Introspection. Supported properties:
  //   "lsmkv.stats"                 human-readable level/compaction summary
  //   "lsmkv.num-files-at-level<N>" file count of level N
  //   "lsmkv.write-amplification"   total bytes written / user bytes
  //   "lsmkv.prometheus"            metrics in Prometheus text format
  virtual bool GetProperty(const Slice& property, std::string* value) = 0;

  // Flushes the active memtable to an L0 SSTable and waits for it.
  virtual Status Flush() = 0;

  // Blocks until no flush or compaction is pending. Used by benchmarks to
  // start every measured phase from the same settled LSM shape.
  virtual Status WaitForCompactions() = 0;

  // Compacts everything down to the bottom-most non-empty level and waits.
  virtual Status CompactAll() = 0;
};

// Deletes the database directory's contents. The DB must be closed.
Status DestroyDB(const std::string& name, const Options& options);

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_DB_H_
