// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Version / VersionSet / Compaction: the metadata side of the LSM tree.
//
// A Version is an immutable snapshot of which SSTables are live at each
// level. Readers grab a shared_ptr<Version> and search it with no lock; a
// flush or compaction never mutates a Version, it builds a new one and
// installs it. Files referenced by any still-live Version are never
// deleted, which is what makes lock-free reads safe while compaction is
// rewriting the tree underneath them.

#ifndef LSMKV_SRC_DB_VERSION_SET_H_
#define LSMKV_SRC_DB_VERSION_SET_H_

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "db/version_edit.h"
#include "lsmkv/options.h"
#include "wal/log_writer.h"

namespace lsmkv {

class Compaction;
class Histogram;
class Iterator;
class TableCache;
class VersionSet;
class WritableFile;

using FileRef = std::shared_ptr<const FileMetaData>;

// Returns the smallest index i such that files[i]->largest >= key, or
// files.size() if there is none. REQUIRES: files sorted and disjoint.
size_t FindFile(const InternalKeyComparator& icmp,
                const std::vector<FileRef>& files, const Slice& key);

class Version {
 public:
  Version(const InternalKeyComparator& icmp,
          std::shared_ptr<TableCache> table_cache)
      : icmp_(icmp), table_cache_(std::move(table_cache)) {}
  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  // Point lookup through the levels: L0 newest file first, then one binary
  // searched file per deeper level. Returns NotFound if absent or deleted.
  Status Get(const ReadOptions& options, const LookupKey& key,
             std::string* value, int* files_probed = nullptr) const;

  // Appends one iterator per L0 file and one concatenating iterator per
  // non-empty deeper level. Merged together they yield this Version's
  // contents.
  void AddIterators(const ReadOptions& options,
                    std::vector<std::unique_ptr<Iterator>>* iters) const;

  int NumFiles(int level) const {
    return static_cast<int>(files_[level].size());
  }
  uint64_t NumLevelBytes(int level) const;
  const std::vector<FileRef>& files(int level) const { return files_[level]; }

  // Files in `level` overlapping [begin, end] (nullptr = unbounded).
  void GetOverlappingInputs(int level, const InternalKey* begin,
                            const InternalKey* end,
                            std::vector<FileRef>* inputs) const;

  std::string DebugString() const;

 private:
  friend class VersionSet;
  friend class Compaction;

  std::unique_ptr<Iterator> NewConcatenatingIterator(
      const ReadOptions& options, int level) const;

  const InternalKeyComparator icmp_;
  const std::shared_ptr<TableCache> table_cache_;
  std::vector<FileRef> files_[config::kNumLevels];

  // Level that most needs compaction and its score (>= 1 means "do it").
  double compaction_score_ = -1;
  int compaction_level_ = -1;
};

class VersionSet {
 public:
  VersionSet(std::string dbname, const Options* options,
             std::shared_ptr<TableCache> table_cache,
             const InternalKeyComparator* icmp);
  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;
  ~VersionSet();

  // Applies *edit to the current version, appends it to the MANIFEST
  // (fsync'd), and installs the result as current.
  // REQUIRES: *lock holds the DB mutex; it is released during the MANIFEST
  // I/O and re-acquired before returning. Only one caller at a time (the
  // background thread, or Open before it starts).
  Status LogAndApply(VersionEdit* edit, std::unique_lock<std::mutex>* lock);

  // Rebuilds state from CURRENT + MANIFEST.
  Status Recover();

  std::shared_ptr<Version> current() const { return current_; }

  uint64_t ManifestFileNumber() const { return manifest_file_number_; }
  uint64_t NewFileNumber() { return next_file_number_++; }
  // Returns a number obtained from NewFileNumber() that went unused.
  void ReuseFileNumber(uint64_t file_number) {
    if (next_file_number_ == file_number + 1) next_file_number_ = file_number;
  }
  void MarkFileNumberUsed(uint64_t number) {
    if (next_file_number_ <= number) next_file_number_ = number + 1;
  }

  int NumLevelFiles(int level) const { return current_->NumFiles(level); }
  uint64_t NumLevelBytes(int level) const {
    return current_->NumLevelBytes(level);
  }

  SequenceNumber LastSequence() const {
    return last_sequence_.load(std::memory_order_acquire);
  }
  void SetLastSequence(SequenceNumber s) {
    last_sequence_.store(s, std::memory_order_release);
  }

  uint64_t LogNumber() const { return log_number_; }

  // Picks the most urgent compaction, or nullptr if nothing needs one.
  std::unique_ptr<Compaction> PickCompaction();
  // A compaction of every file in `level` (manual / CompactAll).
  std::unique_ptr<Compaction> CompactLevel(int level);

  bool NeedsCompaction() const { return current_->compaction_score_ >= 1; }
  double MaxCompactionScore() const { return current_->compaction_score_; }

  // A merged iterator over the inputs of compaction c.
  std::unique_ptr<Iterator> MakeInputIterator(Compaction* c);

  // Adds the number of every file referenced by any live Version.
  void AddLiveFiles(std::set<uint64_t>* live);

  // RocksDB-style estimate of the bytes compaction must still rewrite to
  // bring every level under its target size.
  uint64_t EstimatedPendingCompactionBytes() const;

  uint64_t MaxBytesForLevel(int level) const;

  // Optional: receives the latency of every MANIFEST fsync.
  void SetManifestSyncHistogram(Histogram* h) { manifest_sync_ = h; }

 private:
  class Builder;
  friend class Compaction;

  void Finalize(Version* v) const;
  void SetupOtherInputs(Compaction* c);
  Status WriteSnapshot(log::Writer* log);
  void AppendVersion(std::shared_ptr<Version> v);
  void GetRange(const std::vector<FileRef>& inputs, InternalKey* smallest,
                InternalKey* largest) const;
  void GetRange2(const std::vector<FileRef>& inputs1,
                 const std::vector<FileRef>& inputs2, InternalKey* smallest,
                 InternalKey* largest) const;

  Env* const env_;
  const std::string dbname_;
  const Options* const options_;
  const std::shared_ptr<TableCache> table_cache_;
  const InternalKeyComparator icmp_;
  uint64_t next_file_number_ = 2;
  uint64_t manifest_file_number_ = 0;
  std::atomic<SequenceNumber> last_sequence_{0};
  uint64_t log_number_ = 0;

  // Opened lazily: the first LogAndApply after Open writes a fresh
  // MANIFEST that starts with a full snapshot.
  std::unique_ptr<WritableFile> descriptor_file_;
  std::unique_ptr<log::Writer> descriptor_log_;

  std::shared_ptr<Version> current_;
  // Every Version that may still be in use (for AddLiveFiles).
  std::list<std::weak_ptr<Version>> versions_;

  // Per-level key at which the next compaction at that level should start
  // (round-robin through the key space). Empty = start at the beginning.
  std::string compact_pointer_[config::kNumLevels];
  Histogram* manifest_sync_ = nullptr;
};

// Describes one compaction: inputs from `level` and `level + 1`, output to
// `level + 1`.
class Compaction {
 public:
  Compaction(const Options* options, int level,
             std::shared_ptr<Version> input_version);
  Compaction(const Compaction&) = delete;
  Compaction& operator=(const Compaction&) = delete;

  int level() const { return level_; }
  VersionEdit* edit() { return &edit_; }

  // which = 0: files in level(); which = 1: files in level() + 1.
  int num_input_files(int which) const {
    return static_cast<int>(inputs_[which].size());
  }
  const FileMetaData* input(int which, int i) const {
    return inputs_[which][static_cast<size_t>(i)].get();
  }
  uint64_t InputBytes() const;

  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

  // A single input file with nothing overlapping in the next level can be
  // moved by editing metadata only -- zero bytes rewritten.
  bool IsTrivialMove() const;

  // Adds "delete input file" records to *edit.
  void AddInputDeletions(VersionEdit* edit);

  // True if no level below the output level holds data for user_key, so a
  // tombstone for it has nothing left to shadow and can be dropped.
  // REQUIRES: called with non-decreasing user keys.
  bool IsBaseLevelForKey(const Slice& user_key);

  // True if the current output should be closed before `internal_key`
  // to keep its overlap with level()+2 bounded (limits the cost of the
  // future compaction of this output).
  bool ShouldStopBefore(const Slice& internal_key);

 private:
  friend class VersionSet;

  const int level_;
  const uint64_t max_output_file_size_;
  const uint64_t max_grandparent_overlap_bytes_;
  std::shared_ptr<Version> input_version_;
  VersionEdit edit_;

  std::vector<FileRef> inputs_[2];
  std::vector<FileRef> grandparents_;  // overlapping files in level + 2
  size_t grandparent_index_ = 0;
  bool seen_key_ = false;
  uint64_t overlapped_bytes_ = 0;

  // Per-level cursor for IsBaseLevelForKey.
  size_t level_ptrs_[config::kNumLevels] = {};
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_DB_VERSION_SET_H_
