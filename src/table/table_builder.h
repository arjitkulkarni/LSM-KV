// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_TABLE_TABLE_BUILDER_H_
#define LSMKV_SRC_TABLE_TABLE_BUILDER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "lsmkv/env.h"
#include "lsmkv/status.h"
#include "table/block_builder.h"
#include "table/format.h"

namespace lsmkv {

// Streams sorted key/value pairs into an SSTable file (layout in format.h).
// Memory use is bounded by one data block plus the filter keys, so tables
// of any size can be built.
class TableBuilder {
 public:
  // Does not take ownership of `file`; the caller Syncs and Closes it after
  // Finish().
  TableBuilder(const TableOptions& options, WritableFile* file);
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;
  // REQUIRES: Finish() or Abandon() has been called.
  ~TableBuilder();

  // REQUIRES: key is after any previously added key per the comparator.
  void Add(const Slice& key, const Slice& value);

  // Writes the current data block (if any) to the file.
  void Flush();

  Status status() const { return status_; }

  // Writes filter, index and footer. No further Add() calls allowed.
  Status Finish();

  // The builder's contents should be discarded.
  void Abandon();

  uint64_t NumEntries() const { return num_entries_; }
  // Size of the file generated so far (final size after Finish()).
  uint64_t FileSize() const { return offset_; }

 private:
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  void WriteRawBlock(const Slice& contents, BlockHandle* handle);

  const TableOptions options_;
  WritableFile* const file_;
  uint64_t offset_ = 0;
  Status status_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::string last_key_;
  uint64_t num_entries_ = 0;
  bool closed_ = false;  // Finish() or Abandon() has been called

  // Filter input, stored flat to avoid one allocation per key.
  std::string filter_keys_;
  std::vector<size_t> filter_key_starts_;

  // We do not emit the index entry for a block until we have seen the first
  // key of the next block, so the separator can be shortened: e.g. between
  // "the quick brown fox" and "the who" the index can store "the r".
  bool pending_index_entry_ = false;
  BlockHandle pending_handle_;
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_TABLE_TABLE_BUILDER_H_
