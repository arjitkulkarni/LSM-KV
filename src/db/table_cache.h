// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_DB_TABLE_CACHE_H_
#define LSMKV_SRC_DB_TABLE_CACHE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "lsmkv/cache.h"
#include "lsmkv/env.h"
#include "lsmkv/iterator.h"
#include "lsmkv/options.h"
#include "table/format.h"
#include "table/table.h"

namespace lsmkv {

// Keeps up to `entries` SSTables open (file handle + pinned index/filter),
// evicting the least recently used. Reuses the same LRU Cache
// implementation as the block cache -- one policy class, two call sites.
class TableCache {
 public:
  TableCache(std::string dbname, Env* env, TableOptions options, int entries);
  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // Iterator over file `file_number` (of length `file_size`).
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options,
                                        uint64_t file_number,
                                        uint64_t file_size);

  // Point lookup in one file (see Table::InternalGet).
  Status Get(const ReadOptions& options, uint64_t file_number,
             uint64_t file_size, const Slice& internal_key,
             const std::function<void(const Slice&, const Slice&)>& handle);

  // Opens (or finds) the table. Used after writing a table to verify that
  // it reads back before it is published in a Version.
  Status FindTable(uint64_t file_number, uint64_t file_size,
                   std::shared_ptr<Table>* table);

  // Drops the cached handle for a file about to be deleted.
  void Evict(uint64_t file_number);

  const TableOptions& table_options() const { return options_; }

 private:
  Env* const env_;
  const std::string dbname_;
  const TableOptions options_;
  std::shared_ptr<Cache> cache_;
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_DB_TABLE_CACHE_H_
