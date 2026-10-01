// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "table/table_builder.h"

#include <cassert>

#include "lsmkv/comparator.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsmkv {

TableBuilder::TableBuilder(const TableOptions& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.comparator, options.block_restart_interval),
      // Index entries are rarely adjacent in key space, so prefix
      // compression buys little; a restart per entry makes every binary
      // search probe self-contained.
      index_block_(options.comparator, 1) {}

TableBuilder::~TableBuilder() {
  assert(closed_);  // Catch errors where the caller forgot to call Finish()
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
  assert(!closed_);
  if (!status_.ok()) return;
  if (num_entries_ > 0) {
    assert(options_.comparator->Compare(key, Slice(last_key_)) > 0);
  }

  if (pending_index_entry_) {
    assert(data_block_.empty());
    options_.comparator->FindShortestSeparator(&last_key_, key);
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(last_key_, Slice(handle_encoding));
    pending_index_entry_ = false;
  }

  if (options_.filter_policy != nullptr) {
    filter_key_starts_.push_back(filter_keys_.size());
    filter_keys_.append(key.data(), key.size());
  }

  last_key_.assign(key.data(), key.size());
  num_entries_++;
  data_block_.Add(key, value);

  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    Flush();
  }
}

void TableBuilder::Flush() {
  assert(!closed_);
  if (!status_.ok()) return;
  if (data_block_.empty()) return;
  assert(!pending_index_entry_);
  WriteBlock(&data_block_, &pending_handle_);
  if (status_.ok()) pending_index_entry_ = true;
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  const Slice raw = block->Finish();
  WriteRawBlock(raw, handle);
  block->Reset();
}

void TableBuilder::WriteRawBlock(const Slice& contents, BlockHandle* handle) {
  handle->set_offset(offset_);
  handle->set_size(contents.size());
  status_ = file_->Append(contents);
  if (status_.ok()) {
    char trailer[kBlockTrailerSize];
    trailer[0] = static_cast<char>(kNoCompression);
    uint32_t crc = crc32c::Value(contents.data(), contents.size());
    crc = crc32c::Extend(crc, trailer, 1);  // extend to cover block type
    EncodeFixed32(trailer + 1, crc32c::Mask(crc));
    status_ = file_->Append(Slice(trailer, kBlockTrailerSize));
    if (status_.ok()) offset_ += contents.size() + kBlockTrailerSize;
  }
}

Status TableBuilder::Finish() {
  Flush();
  assert(!closed_);
  closed_ = true;

  BlockHandle filter_block_handle;
  BlockHandle index_block_handle;
  filter_block_handle.set_offset(0);
  filter_block_handle.set_size(0);

  // Filter block: varstring(policy name) | filter bytes.
  if (status_.ok() && options_.filter_policy != nullptr && num_entries_ > 0) {
    std::vector<Slice> keys;
    keys.reserve(filter_key_starts_.size());
    for (size_t i = 0; i < filter_key_starts_.size(); i++) {
      const size_t start = filter_key_starts_[i];
      const size_t end = (i + 1 < filter_key_starts_.size())
                             ? filter_key_starts_[i + 1]
                             : filter_keys_.size();
      keys.emplace_back(filter_keys_.data() + start, end - start);
    }
    std::string block;
    PutLengthPrefixedSlice(&block, options_.filter_policy->Name());
    options_.filter_policy->CreateFilter(keys.data(),
                                         static_cast<int>(keys.size()), &block);
    WriteRawBlock(block, &filter_block_handle);
  }

  // Index block.
  if (status_.ok()) {
    if (pending_index_entry_) {
      options_.comparator->FindShortSuccessor(&last_key_);
      std::string handle_encoding;
      pending_handle_.EncodeTo(&handle_encoding);
      index_block_.Add(last_key_, Slice(handle_encoding));
      pending_index_entry_ = false;
    }
    WriteBlock(&index_block_, &index_block_handle);
  }

  // Footer.
  if (status_.ok()) {
    Footer footer;
    footer.set_filter_handle(filter_block_handle);
    footer.set_index_handle(index_block_handle);
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    status_ = file_->Append(footer_encoding);
    if (status_.ok()) offset_ += footer_encoding.size();
  }
  return status_;
}

void TableBuilder::Abandon() {
  assert(!closed_);
  closed_ = true;
}

}  // namespace lsmkv
