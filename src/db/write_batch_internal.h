// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_DB_WRITE_BATCH_INTERNAL_H_
#define LSMKV_SRC_DB_WRITE_BATCH_INTERNAL_H_

#include <cstdint>

#include "db/dbformat.h"
#include "lsmkv/write_batch.h"

namespace lsmkv {

class MemTable;

// Engine-only operations on a WriteBatch; kept out of the public header.
class WriteBatchInternal {
 public:
  static constexpr size_t kHeader = 12;  // fixed64 sequence + fixed32 count

  static uint32_t Count(const WriteBatch* batch);
  static void SetCount(WriteBatch* batch, uint32_t n);

  static SequenceNumber Sequence(const WriteBatch* batch);
  static void SetSequence(WriteBatch* batch, SequenceNumber seq);

  static Slice Contents(const WriteBatch* batch) { return Slice(batch->rep_); }
  static size_t ByteSize(const WriteBatch* batch) { return batch->rep_.size(); }
  static void SetContents(WriteBatch* batch, const Slice& contents);

  struct InsertStats {
    uint64_t puts = 0;
    uint64_t deletes = 0;
    uint64_t payload_bytes = 0;  // user key + value bytes
  };

  // Inserts every record into `memtable`, numbering them from the batch's
  // sequence. Accumulates counts into *stats when non-null.
  static Status InsertInto(const WriteBatch* batch, MemTable* memtable,
                           InsertStats* stats = nullptr);

  static void Append(WriteBatch* dst, const WriteBatch* src);
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_DB_WRITE_BATCH_INTERNAL_H_
