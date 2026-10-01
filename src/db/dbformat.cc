// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "db/dbformat.h"

#include <cstdio>
#include <vector>

namespace lsmkv {

namespace {

std::string EscapeForDebug(const Slice& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c >= ' ' && c <= '~') {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    }
  }
  return out;
}

}  // namespace

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  const uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  const auto c = static_cast<unsigned char>(num & 0xff);
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  result->user_key = Slice(internal_key.data(), n - 8);
  return (c <= static_cast<unsigned char>(kTypeValue));
}

std::string ParsedInternalKey::DebugString() const {
  return "'" + EscapeForDebug(user_key) + "' @ " + std::to_string(sequence) +
         " : " + (type == kTypeValue ? "val" : "del");
}

std::string InternalKey::DebugString() const {
  ParsedInternalKey parsed;
  if (ParseInternalKey(rep_, &parsed)) return parsed.DebugString();
  return "(bad)" + EscapeForDebug(rep_);
}

const char* InternalKeyComparator::Name() const {
  return "lsmkv.InternalKeyComparator";
}

int InternalKeyComparator::Compare(const Slice& akey, const Slice& bkey) const {
  int r = user_comparator_->Compare(ExtractUserKey(akey), ExtractUserKey(bkey));
  if (r == 0) {
    const uint64_t anum = ExtractTag(akey);
    const uint64_t bnum = ExtractTag(bkey);
    if (anum > bnum) {
      r = -1;  // larger sequence sorts first
    } else if (anum < bnum) {
      r = +1;
    }
  }
  return r;
}

void InternalKeyComparator::FindShortestSeparator(std::string* start,
                                                  const Slice& limit) const {
  // Shorten the user-key portion, then re-attach the earliest possible tag
  // so the result still sorts >= every internal key with that user key.
  const Slice user_start = ExtractUserKey(*start);
  const Slice user_limit = ExtractUserKey(limit);
  std::string tmp(user_start.data(), user_start.size());
  user_comparator_->FindShortestSeparator(&tmp, user_limit);
  if (tmp.size() < user_start.size() &&
      user_comparator_->Compare(user_start, tmp) < 0) {
    PutFixed64(&tmp,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    *start = std::move(tmp);
  }
}

void InternalKeyComparator::FindShortSuccessor(std::string* key) const {
  const Slice user_key = ExtractUserKey(*key);
  std::string tmp(user_key.data(), user_key.size());
  user_comparator_->FindShortSuccessor(&tmp);
  if (tmp.size() < user_key.size() &&
      user_comparator_->Compare(user_key, tmp) < 0) {
    PutFixed64(&tmp,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    *key = std::move(tmp);
  }
}

const char* InternalFilterPolicy::Name() const { return user_policy_->Name(); }

void InternalFilterPolicy::CreateFilter(const Slice* keys, int n,
                                        std::string* dst) const {
  std::vector<Slice> user_keys;
  user_keys.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) user_keys.push_back(ExtractUserKey(keys[i]));
  user_policy_->CreateFilter(user_keys.data(), n, dst);
}

bool InternalFilterPolicy::KeyMayMatch(const Slice& key,
                                       const Slice& filter) const {
  return user_policy_->KeyMayMatch(ExtractUserKey(key), filter);
}

LookupKey::LookupKey(const Slice& user_key, SequenceNumber s) {
  const size_t usize = user_key.size();
  const size_t needed = usize + 13;  // conservative varint + tag
  char* dst = (needed <= sizeof(space_)) ? space_ : new char[needed];
  start_ = dst;
  dst = EncodeVarint32(dst, static_cast<uint32_t>(usize + 8));
  kstart_ = dst;
  if (usize > 0) std::memcpy(dst, user_key.data(), usize);
  dst += usize;
  EncodeFixed64(dst, PackSequenceAndType(s, kValueTypeForSeek));
  dst += 8;
  end_ = dst;
}

LookupKey::~LookupKey() {
  if (start_ != space_) delete[] start_;
}

}  // namespace lsmkv
