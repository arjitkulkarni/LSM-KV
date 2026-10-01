// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Internal keys: how user keys are versioned inside the engine.

#ifndef LSMKV_SRC_DB_DBFORMAT_H_
#define LSMKV_SRC_DB_DBFORMAT_H_

#include <cstdint>
#include <memory>
#include <string>

#include "lsmkv/comparator.h"
#include "lsmkv/filter_policy.h"
#include "lsmkv/slice.h"
#include "util/coding.h"

namespace lsmkv {

// Level layout constants.
namespace config {
constexpr int kNumLevels = 7;
}  // namespace config

enum ValueType : unsigned char {
  kTypeDeletion = 0x0,  // a tombstone
  kTypeValue = 0x1,
};
// When seeking, we want the *newest* entry for a user key. Entries sort by
// descending sequence and then descending type, so seeking with the max
// type value lands on the first (newest) entry.
constexpr ValueType kValueTypeForSeek = kTypeValue;

using SequenceNumber = uint64_t;
// The low 8 bits of the tag hold the type, leaving 56 bits of sequence.
constexpr SequenceNumber kMaxSequenceNumber = ((uint64_t{1} << 56) - 1);

// internal_key := user_key | fixed64(sequence << 8 | type)
//
// Every write gets a unique, monotonically increasing sequence number at
// commit time. Two writes to the same user key therefore become two
// distinct internal keys, ordered newest-first. That single idea is what
// lets memtable inserts run in parallel after group commit (arrival order no
// longer matters, sequence order does), lets tombstones shadow older
// values in lower levels, and lets an iterator filter out writes newer than
// its creation point.
struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeValue;

  ParsedInternalKey() = default;
  ParsedInternalKey(const Slice& u, SequenceNumber seq, ValueType t)
      : user_key(u), sequence(seq), type(t) {}
  std::string DebugString() const;
};

inline uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
  return (seq << 8) | t;
}

inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + 8;
}

void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// Returns false if `internal_key` is malformed.
bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result);

inline Slice ExtractUserKey(const Slice& internal_key) {
  return Slice(internal_key.data(), internal_key.size() - 8);
}

inline uint64_t ExtractTag(const Slice& internal_key) {
  return DecodeFixed64(internal_key.data() + internal_key.size() - 8);
}

// Orders internal keys: user key ascending (via the user comparator), then
// sequence descending. A Decorator over the user's Comparator.
class InternalKeyComparator final : public Comparator {
 public:
  explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c) {}
  // A value type (one pointer), unlike user comparators: components such as
  // each memtable and Version keep their own copy.
  InternalKeyComparator(const InternalKeyComparator& other)
      : Comparator(), user_comparator_(other.user_comparator_) {}
  InternalKeyComparator& operator=(const InternalKeyComparator&) = delete;
  const char* Name() const override;
  int Compare(const Slice& a, const Slice& b) const override;
  void FindShortestSeparator(std::string* start,
                             const Slice& limit) const override;
  void FindShortSuccessor(std::string* key) const override;

  const Comparator* user_comparator() const { return user_comparator_; }

 private:
  const Comparator* user_comparator_;
};

// Filters are built over *user* keys (a Get knows the user key, not the
// sequence number it will find). This Decorator strips the 8-byte tag
// before delegating to the user's policy.
class InternalFilterPolicy final : public FilterPolicy {
 public:
  explicit InternalFilterPolicy(std::shared_ptr<const FilterPolicy> p)
      : user_policy_(std::move(p)) {}
  const char* Name() const override;
  void CreateFilter(const Slice* keys, int n, std::string* dst) const override;
  bool KeyMayMatch(const Slice& key, const Slice& filter) const override;

 private:
  std::shared_ptr<const FilterPolicy> user_policy_;
};

// An owned, encoded internal key (used for file boundaries in metadata).
class InternalKey {
 public:
  InternalKey() = default;
  InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
  }

  bool DecodeFrom(const Slice& s) {
    rep_.assign(s.data(), s.size());
    return !rep_.empty();
  }
  Slice Encode() const { return rep_; }
  Slice user_key() const { return ExtractUserKey(rep_); }
  void SetFrom(const ParsedInternalKey& p) {
    rep_.clear();
    AppendInternalKey(&rep_, p);
  }
  void Clear() { rep_.clear(); }
  bool empty() const { return rep_.empty(); }
  std::string DebugString() const;

 private:
  std::string rep_;
};

inline int CompareInternalKeys(const InternalKeyComparator& icmp,
                               const InternalKey& a, const InternalKey& b) {
  return icmp.Compare(a.Encode(), b.Encode());
}

// A key formatted for a memtable/SSTable point lookup of `user_key` as of
// `sequence`. Owns its bytes; small keys avoid a heap allocation.
//
//   memtable_key := varint32(user_key.size() + 8) | internal_key
class LookupKey {
 public:
  LookupKey(const Slice& user_key, SequenceNumber sequence);
  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;
  ~LookupKey();

  Slice memtable_key() const {
    return Slice(start_, static_cast<size_t>(end_ - start_));
  }
  Slice internal_key() const {
    return Slice(kstart_, static_cast<size_t>(end_ - kstart_));
  }
  Slice user_key() const {
    return Slice(kstart_, static_cast<size_t>(end_ - kstart_ - 8));
  }

 private:
  const char* start_;
  const char* kstart_;
  const char* end_;
  char space_[200];
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_DB_DBFORMAT_H_
