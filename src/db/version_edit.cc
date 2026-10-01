// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "db/version_edit.h"

#include "util/coding.h"

namespace lsmkv {

// Tag numbers for serialized VersionEdit. These numbers are written to
// disk and must not change.
enum Tag : uint32_t {
  kComparator = 1,
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kCompactPointer = 5,
  kDeletedFile = 6,
  kNewFile = 7,
};

void VersionEdit::Clear() {
  comparator_.clear();
  log_number_ = 0;
  last_sequence_ = 0;
  next_file_number_ = 0;
  has_comparator_ = false;
  has_log_number_ = false;
  has_next_file_number_ = false;
  has_last_sequence_ = false;
  compact_pointers_.clear();
  deleted_files_.clear();
  new_files_.clear();
}

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator_) {
    PutVarint32(dst, kComparator);
    PutLengthPrefixedSlice(dst, comparator_);
  }
  if (has_log_number_) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, last_sequence_);
  }
  for (const auto& [level, key] : compact_pointers_) {
    PutVarint32(dst, kCompactPointer);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutLengthPrefixedSlice(dst, key.Encode());
  }
  for (const auto& [level, number] : deleted_files_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, number);
  }
  for (const auto& [level, f] : new_files_) {
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixedSlice(dst, f.smallest.Encode());
    PutLengthPrefixedSlice(dst, f.largest.Encode());
  }
}

namespace {

bool GetInternalKey(Slice* input, InternalKey* dst) {
  Slice str;
  return GetLengthPrefixedSlice(input, &str) && dst->DecodeFrom(str);
}

bool GetLevel(Slice* input, int* level) {
  uint32_t v;
  if (GetVarint32(input, &v) && v < config::kNumLevels) {
    *level = static_cast<int>(v);
    return true;
  }
  return false;
}

}  // namespace

Status VersionEdit::DecodeFrom(const Slice& src) {
  Clear();
  Slice input = src;
  const char* msg = nullptr;
  uint32_t tag;

  // Temporary storage for parsing.
  int level;
  uint64_t number;
  FileMetaData f;
  Slice str;
  InternalKey key;

  while (msg == nullptr && GetVarint32(&input, &tag)) {
    switch (tag) {
      case kComparator:
        if (GetLengthPrefixedSlice(&input, &str)) {
          comparator_ = str.ToString();
          has_comparator_ = true;
        } else {
          msg = "comparator name";
        }
        break;
      case kLogNumber:
        if (GetVarint64(&input, &log_number_)) {
          has_log_number_ = true;
        } else {
          msg = "log number";
        }
        break;
      case kNextFileNumber:
        if (GetVarint64(&input, &next_file_number_)) {
          has_next_file_number_ = true;
        } else {
          msg = "next file number";
        }
        break;
      case kLastSequence:
        if (GetVarint64(&input, &last_sequence_)) {
          has_last_sequence_ = true;
        } else {
          msg = "last sequence number";
        }
        break;
      case kCompactPointer:
        if (GetLevel(&input, &level) && GetInternalKey(&input, &key)) {
          compact_pointers_.emplace_back(level, key);
        } else {
          msg = "compaction pointer";
        }
        break;
      case kDeletedFile:
        if (GetLevel(&input, &level) && GetVarint64(&input, &number)) {
          deleted_files_.insert(std::make_pair(level, number));
        } else {
          msg = "deleted file";
        }
        break;
      case kNewFile:
        if (GetLevel(&input, &level) && GetVarint64(&input, &f.number) &&
            GetVarint64(&input, &f.file_size) &&
            GetInternalKey(&input, &f.smallest) &&
            GetInternalKey(&input, &f.largest)) {
          new_files_.emplace_back(level, f);
        } else {
          msg = "new-file entry";
        }
        break;
      default:
        msg = "unknown tag";
        break;
    }
  }

  if (msg == nullptr && !input.empty()) msg = "invalid tag";
  if (msg != nullptr) return Status::Corruption("VersionEdit", msg);
  return Status::OK();
}

std::string VersionEdit::DebugString() const {
  std::string r = "VersionEdit {";
  if (has_comparator_) r += "\n  Comparator: " + comparator_;
  if (has_log_number_) r += "\n  LogNumber: " + std::to_string(log_number_);
  if (has_next_file_number_) {
    r += "\n  NextFile: " + std::to_string(next_file_number_);
  }
  if (has_last_sequence_) {
    r += "\n  LastSeq: " + std::to_string(last_sequence_);
  }
  for (const auto& [level, key] : compact_pointers_) {
    r += "\n  CompactPointer: L" + std::to_string(level) + " " +
         key.DebugString();
  }
  for (const auto& [level, number] : deleted_files_) {
    r += "\n  RemoveFile: L" + std::to_string(level) + " #" +
         std::to_string(number);
  }
  for (const auto& [level, f] : new_files_) {
    r += "\n  AddFile: L" + std::to_string(level) + " #" +
         std::to_string(f.number) + " " + std::to_string(f.file_size) +
         " bytes [" + f.smallest.DebugString() + " .. " +
         f.largest.DebugString() + "]";
  }
  r += "\n}\n";
  return r;
}

}  // namespace lsmkv
