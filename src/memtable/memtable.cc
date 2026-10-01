// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Entry layout in the arena (one contiguous allocation per insert):
//
//   varint32  internal_key_size   (= user_key.size() + 8)
//   char[]    user_key
//   fixed64   tag                 (sequence << 8 | type)
//   varint32  value_size
//   char[]    value

#include "memtable/memtable.h"

#include <algorithm>
#include <cstring>

#include "table/merger.h"
#include "util/coding.h"
#include "util/hash.h"

namespace lsmkv {

namespace {

Slice GetLengthPrefixedSlice(const char* data) {
  uint32_t len;
  const char* p = data;
  p = GetVarint32PtrFast(p, p + kMaxVarint32Bytes, &len);
  return Slice(p, len);
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  // Internal keys are encoded as length-prefixed strings.
  return comparator->Compare(GetLengthPrefixedSlice(a),
                             GetLengthPrefixedSlice(b));
}

MemTable::MemTable(const InternalKeyComparator& comparator, int num_shards)
    : comparator_(comparator), key_comparator_{&comparator_} {
  const int n = std::clamp(num_shards, 1, 64);
  shards_.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) {
    shards_.push_back(std::make_unique<Shard>(
        key_comparator_, 0x5eed0000ull + static_cast<uint64_t>(i)));
  }
  empty_usage_ = ApproximateMemoryUsage();
}

MemTable::~MemTable() = default;

size_t MemTable::ShardIndex(const Slice& user_key) const {
  if (shards_.size() == 1) return 0;
  return static_cast<size_t>(Hash64(user_key) % shards_.size());
}

size_t MemTable::ApproximateMemoryUsage() const {
  size_t total = 0;
  for (const auto& s : shards_) total += s->arena.MemoryUsage();
  return total;
}

void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key,
                   const Slice& value) {
  const size_t key_size = key.size();
  const size_t val_size = value.size();
  const size_t internal_key_size = key_size + 8;
  const size_t encoded_len =
      static_cast<size_t>(VarintLength(internal_key_size)) + internal_key_size +
      static_cast<size_t>(VarintLength(val_size)) + val_size;

  Shard& shard = *shards_[ShardIndex(key)];
  {
    std::lock_guard<std::mutex> l(shard.mu);
    char* buf = shard.arena.Allocate(encoded_len);
    char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key_size));
    if (key_size > 0) std::memcpy(p, key.data(), key_size);
    p += key_size;
    EncodeFixed64(p, PackSequenceAndType(seq, type));
    p += 8;
    p = EncodeVarint32(p, static_cast<uint32_t>(val_size));
    if (val_size > 0) std::memcpy(p, value.data(), val_size);
    shard.table.Insert(buf);
  }
  num_entries_.fetch_add(1, std::memory_order_relaxed);
  raw_bytes_.fetch_add(key_size + val_size, std::memory_order_relaxed);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) const {
  const Slice memkey = key.memtable_key();
  const Shard& shard = *shards_[ShardIndex(key.user_key())];
  Table::Iterator iter(&shard.table);
  iter.Seek(memkey.data());
  if (!iter.Valid()) return false;

  // The seek landed on the first entry >= (user_key, max sequence). It is
  // the newest version of user_key if the user keys match; otherwise the
  // key is not in this memtable.
  const char* entry = iter.key();
  uint32_t key_length;
  const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
  if (comparator_.user_comparator()->Compare(
          Slice(key_ptr, key_length - 8), key.user_key()) != 0) {
    return false;
  }
  const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
  switch (static_cast<ValueType>(tag & 0xff)) {
    case kTypeValue: {
      const Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
      value->assign(v.data(), v.size());
      return true;
    }
    case kTypeDeletion:
      *s = Status::NotFound(Slice());
      return true;
  }
  return false;
}

// Cursor over one shard. Holds a shared_ptr to the owning MemTable so the
// arena outlives the cursor.
class MemTableIterator final : public Iterator {
 public:
  MemTableIterator(std::shared_ptr<const MemTable> mem,
                   const MemTable::Table* table)
      : mem_(std::move(mem)), iter_(table) {}

  bool Valid() const override { return iter_.Valid(); }
  void Seek(const Slice& k) override { iter_.Seek(EncodeKey(&tmp_, k)); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void Next() override { iter_.Next(); }
  Slice key() const override { return GetLengthPrefixedSlice(iter_.key()); }
  Slice value() const override {
    const Slice key_slice = GetLengthPrefixedSlice(iter_.key());
    return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
  }
  Status status() const override { return Status::OK(); }

 private:
  // Seek targets are internal keys; the skiplist compares length-prefixed
  // entries, so wrap the target the same way.
  static const char* EncodeKey(std::string* scratch, const Slice& target) {
    scratch->clear();
    PutVarint32(scratch, static_cast<uint32_t>(target.size()));
    scratch->append(target.data(), target.size());
    return scratch->data();
  }

  std::shared_ptr<const MemTable> mem_;
  MemTable::Table::Iterator iter_;
  std::string tmp_;
};

std::unique_ptr<Iterator> MemTable::NewIterator() {
  std::shared_ptr<const MemTable> self = shared_from_this();
  if (shards_.size() == 1) {
    return std::make_unique<MemTableIterator>(self, &shards_[0]->table);
  }
  std::vector<std::unique_ptr<Iterator>> children;
  children.reserve(shards_.size());
  for (const auto& s : shards_) {
    children.push_back(std::make_unique<MemTableIterator>(self, &s->table));
  }
  return NewMergingIterator(&comparator_, std::move(children));
}

}  // namespace lsmkv
