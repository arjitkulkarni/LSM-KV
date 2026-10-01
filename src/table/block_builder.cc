// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "table/block_builder.h"

#include <algorithm>
#include <cassert>

#include "lsmkv/comparator.h"
#include "util/coding.h"

namespace lsmkv {

BlockBuilder::BlockBuilder(const Comparator* comparator, int restart_interval)
    : comparator_(comparator), restart_interval_(std::max(1, restart_interval)) {
  restarts_.push_back(0);  // first restart point is at offset 0
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return buffer_.size() + restarts_.size() * sizeof(uint32_t) +
         sizeof(uint32_t);
}

Slice BlockBuilder::Finish() {
  for (const uint32_t restart : restarts_) PutFixed32(&buffer_, restart);
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return Slice(buffer_);
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  assert(!finished_);
  assert(counter_ <= restart_interval_);
  assert(buffer_.empty() || comparator_->Compare(key, Slice(last_key_)) > 0);

  size_t shared = 0;
  if (counter_ < restart_interval_) {
    // How much of the previous key can we reuse?
    const size_t min_length = std::min(last_key_.size(), key.size());
    while (shared < min_length && last_key_[shared] == key[shared]) {
      shared++;
    }
  } else {
    // Restart: store this key in full so a reader can start decoding here.
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(Slice(last_key_) == key);
  counter_++;
}

}  // namespace lsmkv
