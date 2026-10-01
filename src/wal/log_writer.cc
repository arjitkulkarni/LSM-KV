// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "wal/log_writer.h"

#include <cassert>

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsmkv::log {

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(static_cast<int>(dest_length % kBlockSize)) {
  for (int i = 0; i <= kMaxRecordType; i++) {
    const char t = static_cast<char>(i);
    type_crc_[i] = crc32c::Value(&t, 1);
  }
}

Status Writer::AddRecord(const Slice& slice) {
  const char* ptr = slice.data();
  size_t left = slice.size();

  // Fragment the record if necessary and emit it. Note that if slice is
  // empty we still emit a single zero-length FULL record.
  Status s;
  bool begin = true;
  do {
    const int leftover = kBlockSize - block_offset_;
    assert(leftover >= 0);
    if (leftover < kHeaderSize) {
      // Switch to a new block, zero-filling the trailer.
      if (leftover > 0) {
        static_assert(kHeaderSize == 7, "trailer literal must match header");
        s = dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", static_cast<size_t>(leftover)));
        if (!s.ok()) return s;
        bytes_written_ += static_cast<uint64_t>(leftover);
      }
      block_offset_ = 0;
    }

    const size_t avail =
        static_cast<size_t>(kBlockSize - block_offset_ - kHeaderSize);
    const size_t fragment_length = (left < avail) ? left : avail;

    RecordType type;
    const bool end = (left == fragment_length);
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (s.ok() && left > 0);

  // One flush per logical record (not per fragment): the record reaches the
  // OS as a unit, and a group commit of many writers costs one syscall.
  if (s.ok()) s = dest_->Flush();
  return s;
}

Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr,
                                  size_t length) {
  assert(length <= 0xffff);
  assert(block_offset_ + kHeaderSize + static_cast<int>(length) <= kBlockSize);

  char buf[kHeaderSize];
  buf[4] = static_cast<char>(length & 0xff);
  buf[5] = static_cast<char>(length >> 8);
  buf[6] = static_cast<char>(t);

  // CRC covers the type byte and the payload.
  uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
  crc = crc32c::Mask(crc);
  EncodeFixed32(buf, crc);

  Status s = dest_->Append(Slice(buf, kHeaderSize));
  if (s.ok()) s = dest_->Append(Slice(ptr, length));
  block_offset_ += kHeaderSize + static_cast<int>(length);
  bytes_written_ += kHeaderSize + length;
  return s;
}

}  // namespace lsmkv::log
