// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// SSTable file layout:
//
//   [data block 0][trailer]
//   [data block 1][trailer]
//   ...
//   [filter block][trailer]      policy name + one Bloom filter for the file
//   [index block ][trailer]      one entry per data block
//   [footer]                     fixed 48 bytes
//
// trailer := type (1 byte, 0 = uncompressed) | masked crc32c(block + type)
// footer  := filter BlockHandle | index BlockHandle | zero padding to 40 B
//            | magic (fixed64)
//
// A reader opens a table with exactly two small reads (footer, then index +
// filter, which stay pinned in memory), after which any point lookup costs
// at most one data-block read -- zero when the Bloom filter says "absent".

#ifndef LSMKV_SRC_TABLE_FORMAT_H_
#define LSMKV_SRC_TABLE_FORMAT_H_

#include <cstdint>
#include <memory>
#include <string>

#include "lsmkv/env.h"
#include "lsmkv/filter_policy.h"
#include "lsmkv/slice.h"
#include "lsmkv/status.h"

namespace lsmkv {

class Cache;
class Comparator;
class Counter;

// Pointer to a block: its offset and size within the file.
class BlockHandle {
 public:
  // Maximum encoding length of a BlockHandle (two varint64s).
  static constexpr size_t kMaxEncodedLength = 10 + 10;

  uint64_t offset() const { return offset_; }
  void set_offset(uint64_t offset) { offset_ = offset; }
  uint64_t size() const { return size_; }
  void set_size(uint64_t size) { size_ = size; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  uint64_t offset_ = ~uint64_t{0};
  uint64_t size_ = ~uint64_t{0};
};

class Footer {
 public:
  static constexpr size_t kEncodedLength =
      2 * BlockHandle::kMaxEncodedLength + 8;

  const BlockHandle& filter_handle() const { return filter_handle_; }
  void set_filter_handle(const BlockHandle& h) { filter_handle_ = h; }
  const BlockHandle& index_handle() const { return index_handle_; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  BlockHandle filter_handle_;
  BlockHandle index_handle_;
};

// "lsmkvsst" read as a little-endian 64-bit integer.
constexpr uint64_t kTableMagicNumber = 0x7473737674736d6cull;

// 1-byte type + 32-bit crc.
constexpr size_t kBlockTrailerSize = 5;

enum BlockCompressionType : unsigned char { kNoCompression = 0x0 };

// Reads the block identified by `handle` and verifies its checksum.
Status ReadBlock(const RandomAccessFile* file, const BlockHandle& handle,
                 std::string* contents);

// Optional counters the read path bumps. Any pointer may be null.
struct TableReadCounters {
  Counter* bloom_checks = nullptr;     // filter consulted
  Counter* bloom_negatives = nullptr;  // filter said "definitely absent"
  Counter* cache_hits = nullptr;       // data block served from the cache
  Counter* cache_misses = nullptr;
  Counter* block_reads = nullptr;      // data block read from the file
  Counter* block_read_bytes = nullptr;
};

// Everything a TableBuilder/Table needs. Pure data; the DB fills it once.
struct TableOptions {
  const Comparator* comparator = nullptr;  // orders the keys in the file
  std::shared_ptr<const FilterPolicy> filter_policy;  // may be null
  std::shared_ptr<Cache> block_cache;                 // may be null
  size_t block_size = 4096;
  int block_restart_interval = 16;
  TableReadCounters counters;
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_TABLE_FORMAT_H_
