// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "table/block.h"

#include <cassert>

#include "lsmkv/comparator.h"
#include "util/coding.h"

namespace lsmkv {

Block::Block(std::string contents)
    : contents_(std::move(contents)),
      data_(contents_.data()),
      size_(contents_.size()),
      restart_offset_(0) {
  if (size_ < sizeof(uint32_t)) {
    size_ = 0;  // error marker
  } else {
    const size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
    if (NumRestarts() > max_restarts_allowed) {
      size_ = 0;  // the size is too small for NumRestarts()
    } else {
      restart_offset_ = static_cast<uint32_t>(
          size_ - (1 + NumRestarts()) * sizeof(uint32_t));
    }
  }
}

uint32_t Block::NumRestarts() const {
  assert(contents_.size() >= sizeof(uint32_t));
  return DecodeFixed32(data_ + contents_.size() - sizeof(uint32_t));
}

namespace {

// Decodes the three varint32 header fields of the entry at p. Returns
// nullptr on malformed input; never reads at or past `limit`.
inline const char* DecodeEntry(const char* p, const char* limit,
                               uint32_t* shared, uint32_t* non_shared,
                               uint32_t* value_length) {
  if (limit - p < 3) return nullptr;
  *shared = reinterpret_cast<const uint8_t*>(p)[0];
  *non_shared = reinterpret_cast<const uint8_t*>(p)[1];
  *value_length = reinterpret_cast<const uint8_t*>(p)[2];
  if ((*shared | *non_shared | *value_length) < 128) {
    // Fast path: all three values fit in one byte each.
    p += 3;
  } else {
    if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
  }
  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
    return nullptr;
  }
  return p;
}

}  // namespace

Block::Iter::Iter(const Block* block, const Comparator* comparator)
    : comparator_(comparator),
      data_(block->data_),
      restarts_(block->malformed() ? 0 : block->restart_offset_),
      num_restarts_(block->malformed() ? 0 : block->NumRestarts()),
      current_(restarts_),
      restart_index_(num_restarts_) {
  if (block->malformed()) status_ = Status::Corruption("malformed block");
}

uint32_t Block::Iter::GetRestartPoint(uint32_t index) const {
  assert(index < num_restarts_);
  return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
}

void Block::Iter::SeekToRestartPoint(uint32_t index) {
  key_.clear();
  restart_index_ = index;
  // current_ is fixed up by ParseNextKey(); position value_ so that
  // NextEntryOffset() returns the restart offset.
  const uint32_t offset = GetRestartPoint(index);
  value_ = Slice(data_ + offset, 0);
}

void Block::Iter::Next() {
  assert(Valid());
  ParseNextKey();
}

void Block::Iter::Seek(const Slice& target) {
  if (num_restarts_ == 0) {
    current_ = restarts_;
    return;
  }
  // Binary search over the restart array for the last restart point whose
  // key is < target. Restart keys are stored uncompressed, so each probe
  // decodes one entry without any context.
  uint32_t left = 0;
  uint32_t right = num_restarts_ - 1;
  while (left < right) {
    const uint32_t mid = (left + right + 1) / 2;
    const uint32_t region_offset = GetRestartPoint(mid);
    uint32_t shared;
    uint32_t non_shared;
    uint32_t value_length;
    const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_,
                                      &shared, &non_shared, &value_length);
    if (key_ptr == nullptr || (shared != 0)) {
      CorruptionError();
      return;
    }
    const Slice mid_key(key_ptr, non_shared);
    if (comparator_->Compare(mid_key, target) < 0) {
      left = mid;  // everything before "mid" is uninteresting
    } else {
      right = mid - 1;  // everything at or after "mid" is uninteresting
    }
  }

  // Linear scan (at most restart_interval entries) for the first key >=
  // target.
  SeekToRestartPoint(left);
  while (true) {
    if (!ParseNextKey()) return;
    if (comparator_->Compare(Slice(key_), target) >= 0) return;
  }
}

void Block::Iter::SeekToFirst() {
  if (num_restarts_ == 0) {
    current_ = restarts_;
    return;
  }
  SeekToRestartPoint(0);
  ParseNextKey();
}

void Block::Iter::CorruptionError() {
  current_ = restarts_;
  restart_index_ = num_restarts_;
  status_ = Status::Corruption("bad entry in block");
  key_.clear();
  value_.clear();
}

bool Block::Iter::ParseNextKey() {
  current_ = NextEntryOffset();
  const char* p = data_ + current_;
  const char* limit = data_ + restarts_;  // restarts come right after data
  if (p >= limit) {
    // No more entries; mark as invalid.
    current_ = restarts_;
    restart_index_ = num_restarts_;
    return false;
  }

  uint32_t shared;
  uint32_t non_shared;
  uint32_t value_length;
  p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
  if (p == nullptr || key_.size() < shared) {
    CorruptionError();
    return false;
  }
  key_.resize(shared);
  key_.append(p, non_shared);
  value_ = Slice(p + non_shared, value_length);
  shared_ = shared;
  while (restart_index_ + 1 < num_restarts_ &&
         GetRestartPoint(restart_index_ + 1) < current_) {
    ++restart_index_;
  }
  return true;
}

namespace {

class OwningBlockIterator final : public Iterator {
 public:
  OwningBlockIterator(std::shared_ptr<const Block> block,
                      const Comparator* comparator)
      : block_(std::move(block)), iter_(block_.get(), comparator) {}

  bool Valid() const override { return iter_.Valid(); }
  void Seek(const Slice& target) override { iter_.Seek(target); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void Next() override { iter_.Next(); }
  Slice key() const override { return iter_.key(); }
  Slice value() const override { return iter_.value(); }
  Status status() const override { return iter_.status(); }

 private:
  std::shared_ptr<const Block> block_;  // declared first: outlives iter_
  Block::Iter iter_;
};

}  // namespace

std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const Block> block,
                                           const Comparator* comparator) {
  return std::make_unique<OwningBlockIterator>(std::move(block), comparator);
}

}  // namespace lsmkv
