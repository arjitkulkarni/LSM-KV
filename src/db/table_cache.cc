// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "db/table_cache.h"

#include "db/filename.h"
#include "util/coding.h"

namespace lsmkv {

TableCache::TableCache(std::string dbname, Env* env, TableOptions options,
                       int entries)
    : env_(env),
      dbname_(std::move(dbname)),
      options_(std::move(options)),
      // Charge 1 per table, so capacity counts open files. One shard:
      // opening a table is rare next to the reads it serves.
      cache_(NewLRUCache(static_cast<size_t>(entries > 0 ? entries : 1), 0)) {}

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             std::shared_ptr<Table>* table) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  const Slice key(buf, sizeof(buf));
  std::shared_ptr<void> cached = cache_->Lookup(key);
  if (cached != nullptr) {
    *table = std::static_pointer_cast<Table>(cached);
    return Status::OK();
  }

  const std::string fname = TableFileName(dbname_, file_number);
  std::unique_ptr<RandomAccessFile> file;
  Status s = env_->NewRandomAccessFile(fname, &file);
  if (!s.ok()) return s;
  std::shared_ptr<Table> t;
  s = Table::Open(options_, std::move(file), file_size, &t);
  if (!s.ok()) {
    // Do not cache failures: a transient error (EMFILE, say) should not
    // poison the entry. Corruption will simply be rediscovered.
    return s;
  }
  cache_->Insert(key, t, 1);
  *table = std::move(t);
  return Status::OK();
}

std::unique_ptr<Iterator> TableCache::NewIterator(const ReadOptions& options,
                                                  uint64_t file_number,
                                                  uint64_t file_size) {
  std::shared_ptr<Table> table;
  Status s = FindTable(file_number, file_size, &table);
  if (!s.ok()) return NewErrorIterator(s);
  return table->NewIterator(options);
}

Status TableCache::Get(
    const ReadOptions& options, uint64_t file_number, uint64_t file_size,
    const Slice& internal_key,
    const std::function<void(const Slice&, const Slice&)>& handle) {
  std::shared_ptr<Table> table;
  Status s = FindTable(file_number, file_size, &table);
  if (!s.ok()) return s;
  return table->InternalGet(options, internal_key, handle);
}

void TableCache::Evict(uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace lsmkv
