// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_TABLE_BLOCK_BUILDER_H_
#define LSMKV_SRC_TABLE_BLOCK_BUILDER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "lsmkv/slice.h"

namespace lsmkv {

class Comparator;

// Builds a sorted block with prefix-compressed keys.
//
// Sorted keys share long prefixes ("user:000123", "user:000124"), so each
// entry stores only the bytes that differ from the previous key:
//
//   shared_bytes   : varint32  -- prefix length shared with previous key
//   unshared_bytes : varint32
//   value_length   : varint32
//   key_delta      : char[unshared_bytes]
//   value          : char[value_length]
//
// Every `restart_interval` entries the prefix sharing is reset and the
// entry's offset is recorded as a *restart point*. The trailer
//
//   restarts     : uint32[num_restarts]
//   num_restarts : uint32
//
// lets a reader binary-search the restart points (whose keys are stored in
// full) and then scan at most `restart_interval` entries linearly.
class BlockBuilder {
 public:
  BlockBuilder(const Comparator* comparator, int restart_interval);
  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  void Reset();

  // REQUIRES: Finish() has not been called since the last Reset().
  // REQUIRES: key is larger than any previously added key.
  void Add(const Slice& key, const Slice& value);

  // Returns a slice referring to the finished block contents; valid until
  // Reset() or destruction.
  Slice Finish();

  // Estimated size of the block being built (uncompressed).
  size_t CurrentSizeEstimate() const;

  bool empty() const { return buffer_.empty(); }

 private:
  const Comparator* comparator_;
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;  // entries emitted since the last restart
  bool finished_ = false;
  std::string last_key_;
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_TABLE_BLOCK_BUILDER_H_
