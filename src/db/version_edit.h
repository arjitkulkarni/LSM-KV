// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_DB_VERSION_EDIT_H_
#define LSMKV_SRC_DB_VERSION_EDIT_H_

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "lsmkv/status.h"

namespace lsmkv {

// Metadata of one live SSTable. Immutable once published in a Version;
// shared (via shared_ptr) by every Version that contains the file.
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  InternalKey smallest;  // smallest internal key in the table
  InternalKey largest;   // largest internal key in the table
};

// A delta between two Versions: files added, files removed, and the
// counters that must survive a restart. The MANIFEST is nothing but a log
// of encoded VersionEdits; replaying it from the start rebuilds the live
// file set. Because an edit is appended (and fsync'd) as one checksummed
// log record, a flush or compaction becomes visible atomically: after a
// crash either all of its adds and deletes are present or none are.
class VersionEdit {
 public:
  VersionEdit() { Clear(); }

  void Clear();

  void SetComparatorName(const Slice& name) {
    has_comparator_ = true;
    comparator_ = name.ToString();
  }
  // WALs numbered below this are fully reflected in SSTables.
  void SetLogNumber(uint64_t num) {
    has_log_number_ = true;
    log_number_ = num;
  }
  void SetNextFile(uint64_t num) {
    has_next_file_number_ = true;
    next_file_number_ = num;
  }
  void SetLastSequence(SequenceNumber seq) {
    has_last_sequence_ = true;
    last_sequence_ = seq;
  }
  void SetCompactPointer(int level, const InternalKey& key) {
    compact_pointers_.emplace_back(level, key);
  }

  void AddFile(int level, uint64_t file, uint64_t file_size,
               const InternalKey& smallest, const InternalKey& largest) {
    FileMetaData f;
    f.number = file;
    f.file_size = file_size;
    f.smallest = smallest;
    f.largest = largest;
    new_files_.emplace_back(level, f);
  }

  void RemoveFile(int level, uint64_t file) {
    deleted_files_.insert(std::make_pair(level, file));
  }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& src);

  std::string DebugString() const;

  bool has_log_number() const { return has_log_number_; }
  uint64_t log_number() const { return log_number_; }
  const std::vector<std::pair<int, FileMetaData>>& new_files() const {
    return new_files_;
  }
  const std::set<std::pair<int, uint64_t>>& deleted_files() const {
    return deleted_files_;
  }

 private:
  friend class VersionSet;

  std::string comparator_;
  uint64_t log_number_;
  uint64_t next_file_number_;
  SequenceNumber last_sequence_;
  bool has_comparator_;
  bool has_log_number_;
  bool has_next_file_number_;
  bool has_last_sequence_;

  std::vector<std::pair<int, InternalKey>> compact_pointers_;
  std::set<std::pair<int, uint64_t>> deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_DB_VERSION_EDIT_H_
