// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "lsmkv/write_batch.h"

#include "db/dbformat.h"
#include "db/write_batch_internal.h"
#include "memtable/memtable.h"
#include "util/coding.h"

namespace lsmkv {

WriteBatch::Handler::~Handler() = default;

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(WriteBatchInternal::kHeader);
}

uint32_t WriteBatch::Count() const { return WriteBatchInternal::Count(this); }

void WriteBatch::Put(const Slice& key, const Slice& value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice& key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

void WriteBatch::Append(const WriteBatch& source) {
  WriteBatchInternal::Append(this, &source);
}

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < WriteBatchInternal::kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  input.remove_prefix(WriteBatchInternal::kHeader);
  Slice key;
  Slice value;
  uint32_t found = 0;
  while (!input.empty()) {
    found++;
    const char tag = input[0];
    input.remove_prefix(1);
    switch (tag) {
      case kTypeValue:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          handler->Put(key, value);
        } else {
          return Status::Corruption("bad WriteBatch Put");
        }
        break;
      case kTypeDeletion:
        if (GetLengthPrefixedSlice(&input, &key)) {
          handler->Delete(key);
        } else {
          return Status::Corruption("bad WriteBatch Delete");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("WriteBatch has wrong count");
  }
  return Status::OK();
}

uint32_t WriteBatchInternal::Count(const WriteBatch* b) {
  return DecodeFixed32(b->rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* b, uint32_t n) {
  EncodeFixed32(&b->rep_[8], n);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return DecodeFixed64(b->rep_.data());
}

void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  EncodeFixed64(&b->rep_[0], seq);
}

void WriteBatchInternal::SetContents(WriteBatch* b, const Slice& contents) {
  b->rep_.assign(contents.data(), contents.size());
}

namespace {

class MemTableInserter final : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber seq, MemTable* mem)
      : sequence_(seq), mem_(mem) {}

  void Put(const Slice& key, const Slice& value) override {
    mem_->Add(sequence_, kTypeValue, key, value);
    sequence_++;
    stats_.puts++;
    stats_.payload_bytes += key.size() + value.size();
  }
  void Delete(const Slice& key) override {
    mem_->Add(sequence_, kTypeDeletion, key, Slice());
    sequence_++;
    stats_.deletes++;
    stats_.payload_bytes += key.size();
  }
  const WriteBatchInternal::InsertStats& stats() const { return stats_; }

 private:
  SequenceNumber sequence_;
  MemTable* mem_;
  WriteBatchInternal::InsertStats stats_;
};

}  // namespace

Status WriteBatchInternal::InsertInto(const WriteBatch* b, MemTable* memtable,
                                      InsertStats* stats) {
  MemTableInserter inserter(WriteBatchInternal::Sequence(b), memtable);
  Status s = b->Iterate(&inserter);
  if (stats != nullptr) {
    stats->puts += inserter.stats().puts;
    stats->deletes += inserter.stats().deletes;
    stats->payload_bytes += inserter.stats().payload_bytes;
  }
  return s;
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}

}  // namespace lsmkv
