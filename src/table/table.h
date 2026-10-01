// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_TABLE_TABLE_H_
#define LSMKV_SRC_TABLE_TABLE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "lsmkv/iterator.h"
#include "lsmkv/options.h"
#include "table/block.h"
#include "table/format.h"

namespace lsmkv {

// An open, immutable SSTable. The index block and the Bloom filter are
// loaded once at Open() and pinned in memory; data blocks are fetched on
// demand through the (optional, shared) block cache.
//
// Thread-safe: any number of threads may read one Table concurrently.
// Always owned by std::shared_ptr so that iterators can keep it (and its
// file handle) alive after the table cache has evicted it.
class Table : public std::enable_shared_from_this<Table> {
 public:
  static Status Open(const TableOptions& options,
                     std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
                     std::shared_ptr<Table>* table);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;
  ~Table();

  // Iterator over the whole table.
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) const;

  // Point lookup: if the filter admits `key`, seeks to the first entry >=
  // `key` and calls handle_result(entry_key, entry_value). Does nothing if
  // the filter rules the key out or no such entry exists.
  Status InternalGet(
      const ReadOptions& options, const Slice& key,
      const std::function<void(const Slice&, const Slice&)>& handle_result)
      const;

  // True if the Bloom filter (if any) admits the key. Exposed so the Bloom
  // sweep benchmark can measure the raw false-positive rate.
  bool KeyMayMatch(const Slice& key) const;

  // Introspection (lsmkv-dump).
  const Footer& footer() const { return footer_; }
  const std::shared_ptr<const Block>& index_block() const { return index_block_; }
  Slice filter_data() const { return Slice(filter_data_); }
  const std::string& filter_policy_name() const { return filter_name_; }
  uint64_t file_size() const { return file_size_; }
  Status ReadBlockForDump(const Slice& index_value,
                          std::shared_ptr<const Block>* block) const;

 private:
  explicit Table(const TableOptions& options) : options_(options) {}

  // Returns the data block named by an index entry value, from the cache
  // when possible.
  std::shared_ptr<const Block> ReadDataBlock(const ReadOptions& options,
                                             const Slice& index_value,
                                             Status* s) const;

  TableOptions options_;
  std::unique_ptr<RandomAccessFile> file_;
  uint64_t file_size_ = 0;
  uint64_t cache_id_ = 0;
  Footer footer_;
  std::shared_ptr<const Block> index_block_;
  std::string filter_name_;
  std::string filter_data_;  // empty => no usable filter
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_TABLE_TABLE_H_
