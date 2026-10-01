// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_WAL_LOG_WRITER_H_
#define LSMKV_SRC_WAL_LOG_WRITER_H_

#include <cstdint>

#include "lsmkv/env.h"
#include "lsmkv/slice.h"
#include "lsmkv/status.h"
#include "wal/log_format.h"

namespace lsmkv::log {

// Appends length-prefixed, checksummed records to a WritableFile.
// Not thread-safe: the DB guarantees a single writer (the group-commit
// leader) at any time.
class Writer {
 public:
  // `dest` must be empty, or `dest_length` must be its current length (used
  // when an existing WAL is reopened for append after recovery). `dest`
  // must outlive the Writer.
  explicit Writer(WritableFile* dest, uint64_t dest_length = 0);
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  // Appends one logical record and pushes it to the OS (Flush), so once
  // this returns OK the record survives a crash of this process. Sync()
  // on the underlying file is the caller's decision (WriteOptions::sync).
  Status AddRecord(const Slice& slice);

  uint64_t bytes_written() const { return bytes_written_; }

 private:
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* const dest_;
  int block_offset_;  // current offset within the block
  uint64_t bytes_written_ = 0;

  // crc32c of each type byte, precomputed so the payload CRC can be
  // extended from it.
  uint32_t type_crc_[kMaxRecordType + 1];
};

}  // namespace lsmkv::log

#endif  // LSMKV_SRC_WAL_LOG_WRITER_H_
