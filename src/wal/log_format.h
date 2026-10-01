// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Physical format of the write-ahead log (and the MANIFEST, which reuses it).
//
// The file is a sequence of 32 KB blocks. Each block holds records:
//
//   +----------+-----------+-----------+--- ... ---+
//   | CRC (4B) | Size (2B) | Type (1B) | Payload   |
//   +----------+-----------+-----------+--- ... ---+
//
//   CRC  = masked crc32c(type byte + payload), little-endian
//   Size = payload length, little-endian
//   Type = FULL | FIRST | MIDDLE | LAST
//
// A logical record larger than the space left in a block is split into a
// FIRST fragment, zero or more MIDDLE fragments and a LAST fragment, one per
// block. If fewer than 7 bytes (a header) remain in a block they are zero
// padding and the reader skips them.
//
// Why blocks? After damage the reader can resynchronize at the next 32 KB
// boundary instead of losing the rest of the file, and a record header can
// never straddle a block, which bounds how much a single torn write can
// affect.

#ifndef LSMKV_SRC_WAL_LOG_FORMAT_H_
#define LSMKV_SRC_WAL_LOG_FORMAT_H_

namespace lsmkv::log {

enum RecordType : unsigned char {
  // Reserved: an all-zero header means "zero-filled region" (preallocated
  // or torn space), never a real record.
  kZeroType = 0,

  kFullType = 1,

  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,
};
constexpr int kMaxRecordType = kLastType;

constexpr int kBlockSize = 32768;

// Header is checksum (4 bytes), length (2 bytes), type (1 byte).
constexpr int kHeaderSize = 4 + 2 + 1;

}  // namespace lsmkv::log

#endif  // LSMKV_SRC_WAL_LOG_FORMAT_H_
