// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// WriteBatch: an ordered group of updates applied atomically w.r.t. crashes.

#ifndef LSMKV_INCLUDE_LSMKV_WRITE_BATCH_H_
#define LSMKV_INCLUDE_LSMKV_WRITE_BATCH_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "lsmkv/slice.h"
#include "lsmkv/status.h"

namespace lsmkv {

// A batch is encoded exactly as it is logged:
//
//   sequence : fixed64   (first sequence number, assigned at commit time)
//   count    : fixed32
//   record*  : kTypeValue    varstring key  varstring value
//            | kTypeDeletion varstring key
//
// so group commit concatenates batches into one WAL record without
// re-encoding, and recovery replays that record through the same Handler
// interface the memtable uses on the write path.
class WriteBatch {
 public:
  // Visitor over the records of a batch.
  class Handler {
   public:
    virtual ~Handler();
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };

  WriteBatch();
  // Rule of zero: copies and moves are the std::string's.

  void Put(const Slice& key, const Slice& value);
  void Delete(const Slice& key);
  void Clear();

  // Appends the records of `source` (sequence number is not copied).
  void Append(const WriteBatch& source);

  uint32_t Count() const;
  size_t ApproximateSize() const { return rep_.size(); }

  // Replays every record through `handler`; fails on a malformed encoding.
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;
  std::string rep_;
};

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_WRITE_BATCH_H_
