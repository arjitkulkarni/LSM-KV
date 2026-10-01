// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_WAL_LOG_READER_H_
#define LSMKV_SRC_WAL_LOG_READER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "lsmkv/env.h"
#include "lsmkv/slice.h"
#include "lsmkv/status.h"
#include "wal/log_format.h"

namespace lsmkv::log {

// Reads the records written by log::Writer, verifying every checksum.
//
// Damage comes in two flavours the reader deliberately keeps apart:
//
//  * A *torn tail*: the file simply ends in the middle of a header, a
//    payload, or a fragmented record. That is what a crash during an
//    append leaves behind; it is not reported as corruption (the write was
//    never acknowledged). truncated_tail() tells the caller it happened.
//
//  * *Corruption*: a checksum mismatch, an impossible length, a zero-filled
//    header, or fragments out of order. Reported through Reporter, after
//    which the reader resynchronizes at the next 32 KB block and keeps
//    going -- so the caller can see whether valid records follow the
//    damage (mid-log corruption) or not (a damaged tail).
class Reader {
 public:
  class Reporter {
   public:
    virtual ~Reporter();
    // `bytes` is the approximate number of bytes dropped.
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  // Does not take ownership of `file` or `reporter`; both must outlive the
  // Reader. `reporter` may be nullptr.
  Reader(SequentialFile* file, Reporter* reporter, bool checksum);
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  ~Reader();

  // Reads the next complete record into *record. Returns false at EOF.
  // *record stays valid until the next call or a change to *scratch.
  bool ReadRecord(Slice* record, std::string* scratch);

  // File offset of the first byte of the last record returned.
  uint64_t LastRecordOffset() const { return last_record_offset_; }
  // File offset just past the last byte of the last record returned; the
  // length to truncate the file to in order to drop a damaged tail.
  uint64_t LastRecordEndOffset() const { return last_record_end_offset_; }
  // True if the file ended inside a record (a torn final write).
  bool truncated_tail() const { return truncated_tail_; }

 private:
  // Extended record types used internally.
  enum : unsigned int {
    kEof = kMaxRecordType + 1,
    // A malformed physical record (reported via Reporter).
    kBadRecord = kMaxRecordType + 2,
  };

  unsigned int ReadPhysicalRecord(Slice* result);
  void ReportCorruption(uint64_t bytes, const char* reason);
  void ReportDrop(uint64_t bytes, const Status& reason);

  SequentialFile* const file_;
  Reporter* const reporter_;
  const bool checksum_;
  std::unique_ptr<char[]> backing_store_;
  Slice buffer_;
  bool eof_ = false;  // last Read() returned < kBlockSize bytes
  bool truncated_tail_ = false;

  uint64_t last_record_offset_ = 0;
  uint64_t last_record_end_offset_ = 0;
  // Offset of the first location past the end of buffer_.
  uint64_t end_of_buffer_offset_ = 0;
};

}  // namespace lsmkv::log

#endif  // LSMKV_SRC_WAL_LOG_READER_H_
