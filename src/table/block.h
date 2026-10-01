// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_TABLE_BLOCK_H_
#define LSMKV_SRC_TABLE_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "lsmkv/iterator.h"

namespace lsmkv {

class Comparator;

// An immutable, parsed block (see block_builder.h for the layout). Owns its
// bytes. Shared through std::shared_ptr between the block cache and any
// iterators reading it.
class Block {
 public:
  explicit Block(std::string contents);
  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  size_t size() const { return size_; }
  uint32_t NumRestarts() const;
  bool malformed() const { return size_ == 0; }

  // A cursor over the block. Holds a raw pointer: the caller keeps the
  // Block alive (NewBlockIterator below does that with a shared_ptr).
  class Iter;

 private:
  friend class Iter;
  std::string contents_;
  const char* data_;
  size_t size_;
  uint32_t restart_offset_;  // offset in data_ of the restart array
};

class Block::Iter final : public Iterator {
 public:
  Iter(const Block* block, const Comparator* comparator);

  bool Valid() const override { return current_ < restarts_; }
  Status status() const override { return status_; }
  Slice key() const override { return Slice(key_); }
  Slice value() const override { return value_; }

  void Next() override;
  void Seek(const Slice& target) override;
  void SeekToFirst() override;

  // Introspection for lsmkv-dump.
  uint32_t current_offset() const { return current_; }
  uint32_t shared_bytes() const { return shared_; }

 private:
  uint32_t NextEntryOffset() const {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }
  uint32_t GetRestartPoint(uint32_t index) const;
  void SeekToRestartPoint(uint32_t index);
  void CorruptionError();
  bool ParseNextKey();

  const Comparator* const comparator_;
  const char* const data_;      // underlying block contents
  uint32_t const restarts_;     // offset of restart array (list of fixed32)
  uint32_t const num_restarts_;

  uint32_t current_;        // offset of current entry; >= restarts_ if !Valid
  uint32_t restart_index_;  // index of restart block containing current_
  std::string key_;
  Slice value_;
  uint32_t shared_ = 0;
  Status status_;
};

// Iterator that owns a reference to its block.
std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const Block> block,
                                           const Comparator* comparator);

}  // namespace lsmkv

#endif  // LSMKV_SRC_TABLE_BLOCK_H_
