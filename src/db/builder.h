// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_DB_BUILDER_H_
#define LSMKV_SRC_DB_BUILDER_H_

#include <string>

#include "db/dbformat.h"
#include "lsmkv/status.h"
#include "table/format.h"

namespace lsmkv {

class Env;
class Iterator;
class TableCache;
struct FileMetaData;

struct BuildTableStats {
  uint64_t entries_in = 0;
  uint64_t entries_out = 0;  // after dropping shadowed versions
  uint64_t sync_nanos = 0;   // time spent in the table's fsync
};

// Writes the contents of *iter (sorted internal keys) to a new SSTable
// named by meta->number, keeping only the newest entry for each user key
// (there are no snapshots older than the memtable, so older versions are
// unreachable). Tombstones are kept: older values may live in lower levels.
//
// On success, meta is filled in; if the iterator was empty no file is
// created and meta->file_size is 0. The file is fsync'd and re-opened
// through the table cache before this returns, so a table is never
// published in a Version unless it is durable and readable.
Status BuildTable(const std::string& dbname, Env* env,
                  const TableOptions& options,
                  const InternalKeyComparator& icmp, TableCache* table_cache,
                  Iterator* iter, FileMetaData* meta, BuildTableStats* stats);

}  // namespace lsmkv

#endif  // LSMKV_SRC_DB_BUILDER_H_
