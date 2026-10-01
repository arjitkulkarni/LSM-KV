// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "db/version_set.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>

#include "db/filename.h"
#include "db/table_cache.h"
#include "lsmkv/env.h"
#include "lsmkv/metrics.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "wal/log_reader.h"

namespace lsmkv {

namespace {

uint64_t TotalFileSize(const std::vector<FileRef>& files) {
  uint64_t sum = 0;
  for (const auto& f : files) sum += f->file_size;
  return sum;
}

// Iterates the files of one level (sorted, disjoint). key() is a file's
// largest internal key; value() encodes (file number, file size) so the
// second level of a TwoLevelIterator can open the table.
class LevelFileNumIterator final : public Iterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator& icmp,
                       std::vector<FileRef> files)
      : icmp_(icmp), flist_(std::move(files)), index_(flist_.size()) {}

  bool Valid() const override { return index_ < flist_.size(); }
  void Seek(const Slice& target) override {
    index_ = FindFile(icmp_, flist_, target);
  }
  void SeekToFirst() override { index_ = 0; }
  void Next() override {
    assert(Valid());
    index_++;
  }
  Slice key() const override {
    assert(Valid());
    return flist_[index_]->largest.Encode();
  }
  Slice value() const override {
    assert(Valid());
    EncodeFixed64(value_buf_, flist_[index_]->number);
    EncodeFixed64(value_buf_ + 8, flist_[index_]->file_size);
    return Slice(value_buf_, sizeof(value_buf_));
  }
  Status status() const override { return Status::OK(); }

 private:
  const InternalKeyComparator icmp_;
  const std::vector<FileRef> flist_;  // copies pin the metadata
  size_t index_;
  mutable char value_buf_[16];
};

BlockFunction FileOpener(std::shared_ptr<TableCache> cache,
                         const ReadOptions& options) {
  return [cache = std::move(cache),
          options](const Slice& file_value) -> std::unique_ptr<Iterator> {
    if (file_value.size() != 16) {
      return NewErrorIterator(
          Status::Corruption("FileReader invoked with unexpected value"));
    }
    return cache->NewIterator(options, DecodeFixed64(file_value.data()),
                              DecodeFixed64(file_value.data() + 8));
  };
}

// Newest L0 file first.
bool NewestFirst(const FileMetaData* a, const FileMetaData* b) {
  return a->number > b->number;
}

class ManifestReporter final : public log::Reader::Reporter {
 public:
  void Corruption(size_t, const Status& s) override {
    if (status.ok()) status = s;
  }
  Status status;
};

}  // namespace

size_t FindFile(const InternalKeyComparator& icmp,
                const std::vector<FileRef>& files, const Slice& key) {
  size_t left = 0;
  size_t right = files.size();
  while (left < right) {
    const size_t mid = (left + right) / 2;
    if (icmp.Compare(files[mid]->largest.Encode(), key) < 0) {
      // Key at "mid.largest" is < "target". Therefore all files at or
      // before "mid" are uninteresting.
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  return right;
}

// ---- Version ------------------------------------------------------------------

uint64_t Version::NumLevelBytes(int level) const {
  return TotalFileSize(files_[level]);
}

Status Version::Get(const ReadOptions& options, const LookupKey& k,
                    std::string* value, int* files_probed) const {
  const Slice ikey = k.internal_key();
  const Slice user_key = k.user_key();
  const Comparator* ucmp = icmp_.user_comparator();

  enum class State { kNotFound, kFound, kDeleted, kCorrupt };
  State state = State::kNotFound;
  auto handle = [&](const Slice& found_key, const Slice& found_value) {
    ParsedInternalKey parsed;
    if (!ParseInternalKey(found_key, &parsed)) {
      state = State::kCorrupt;
    } else if (ucmp->Compare(parsed.user_key, user_key) == 0) {
      state = (parsed.type == kTypeValue) ? State::kFound : State::kDeleted;
      if (state == State::kFound) {
        value->assign(found_value.data(), found_value.size());
      }
    }
  };

  // Returns true when the search is over (found, deleted, or error).
  Status status;
  auto search = [&](const FileMetaData* f) -> bool {
    if (files_probed != nullptr) (*files_probed)++;
    state = State::kNotFound;
    Status s =
        table_cache_->Get(options, f->number, f->file_size, ikey, handle);
    if (!s.ok()) {
      status = s;
      return true;
    }
    switch (state) {
      case State::kNotFound:
        return false;
      case State::kFound:
        status = Status::OK();
        return true;
      case State::kDeleted:
        status = Status::NotFound(Slice());
        return true;
      case State::kCorrupt:
        status = Status::Corruption("corrupted key for ", user_key);
        return true;
    }
    return false;
  };

  // Level 0: files may overlap, so check every file whose range covers the
  // key, newest first. The first hit is the newest version.
  // Candidates live on the stack in the common case (L0 is kept small by
  // the write-stall triggers), so a point lookup does not allocate.
  const FileMetaData* stack_candidates[32];
  std::vector<const FileMetaData*> heap_candidates;
  const FileMetaData** l0 = stack_candidates;
  if (files_[0].size() > 32) {
    heap_candidates.resize(files_[0].size());
    l0 = heap_candidates.data();
  }
  size_t n0 = 0;
  for (const auto& f : files_[0]) {
    if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
        ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
      l0[n0++] = f.get();
    }
  }
  std::sort(l0, l0 + n0, NewestFirst);
  for (size_t i = 0; i < n0; i++) {
    if (search(l0[i])) return status;
  }

  // Deeper levels: disjoint files, so at most one candidate per level,
  // found by binary search on the files' largest keys.
  for (int level = 1; level < config::kNumLevels; level++) {
    const std::vector<FileRef>& files = files_[level];
    if (files.empty()) continue;
    const size_t index = FindFile(icmp_, files, ikey);
    if (index < files.size()) {
      const FileMetaData* f = files[index].get();
      if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0) {
        if (search(f)) return status;
      }
    }
  }
  return Status::NotFound(Slice());
}

std::unique_ptr<Iterator> Version::NewConcatenatingIterator(
    const ReadOptions& options, int level) const {
  return NewTwoLevelIterator(
      std::make_unique<LevelFileNumIterator>(icmp_, files_[level]),
      FileOpener(table_cache_, options));
}

void Version::AddIterators(const ReadOptions& options,
                           std::vector<std::unique_ptr<Iterator>>* iters) const {
  std::vector<const FileMetaData*> l0;
  for (const auto& f : files_[0]) l0.push_back(f.get());
  std::sort(l0.begin(), l0.end(), NewestFirst);
  for (const FileMetaData* f : l0) {
    iters->push_back(table_cache_->NewIterator(options, f->number, f->file_size));
  }
  // Deeper levels: one lazily-opening concatenating iterator per level.
  for (int level = 1; level < config::kNumLevels; level++) {
    if (!files_[level].empty()) {
      iters->push_back(NewConcatenatingIterator(options, level));
    }
  }
}

void Version::GetOverlappingInputs(int level, const InternalKey* begin,
                                   const InternalKey* end,
                                   std::vector<FileRef>* inputs) const {
  inputs->clear();
  const Comparator* ucmp = icmp_.user_comparator();
  for (const auto& f : files_[level]) {
    if (end != nullptr &&
        ucmp->Compare(f->smallest.user_key(), end->user_key()) > 0) {
      continue;  // file lies entirely after the range
    }
    if (begin != nullptr &&
        ucmp->Compare(f->largest.user_key(), begin->user_key()) < 0) {
      continue;  // file lies entirely before the range
    }
    inputs->push_back(f);
  }
}

std::string Version::DebugString() const {
  std::string r;
  for (int level = 0; level < config::kNumLevels; level++) {
    r += "--- level " + std::to_string(level) + " ---\n";
    for (const auto& f : files_[level]) {
      r += " " + std::to_string(f->number) + ":" +
           std::to_string(f->file_size) + "[" + f->smallest.DebugString() +
           " .. " + f->largest.DebugString() + "]\n";
    }
  }
  return r;
}

// ---- VersionSet::Builder ------------------------------------------------------------

// Accumulates a sequence of edits on top of a base Version without creating
// intermediate Versions (recovery may replay thousands of edits).
class VersionSet::Builder {
 public:
  Builder(VersionSet* vset, const Version* base) : vset_(vset), base_(base) {}

  void Apply(const VersionEdit* edit) {
    for (const auto& [level, key] : edit->compact_pointers_) {
      vset_->compact_pointer_[level] = key.Encode().ToString();
    }
    for (const auto& [level, number] : edit->deleted_files_) {
      levels_[level].deleted.insert(number);
    }
    for (const auto& [level, f] : edit->new_files_) {
      levels_[level].deleted.erase(f.number);
      levels_[level].added[f.number] = std::make_shared<const FileMetaData>(f);
    }
  }

  void SaveTo(Version* v) const {
    const InternalKeyComparator& icmp = vset_->icmp_;
    for (int level = 0; level < config::kNumLevels; level++) {
      const LevelState& st = levels_[level];
      std::vector<FileRef> merged;
      if (base_ != nullptr) {
        for (const auto& f : base_->files_[level]) {
          if (st.deleted.count(f->number) == 0 &&
              st.added.count(f->number) == 0) {
            merged.push_back(f);
          }
        }
      }
      for (const auto& [number, f] : st.added) {
        if (st.deleted.count(number) == 0) merged.push_back(f);
      }
      if (level == 0) {
        std::sort(merged.begin(), merged.end(),
                  [](const FileRef& a, const FileRef& b) {
                    return a->number < b->number;
                  });
      } else {
        std::sort(merged.begin(), merged.end(),
                  [&icmp](const FileRef& a, const FileRef& b) {
                    const int r = icmp.Compare(a->smallest.Encode(),
                                               b->smallest.Encode());
                    return r != 0 ? r < 0 : a->number < b->number;
                  });
        // Invariant of leveled compaction: files in L1+ are disjoint.
        for (size_t i = 1; i < merged.size(); i++) {
          if (icmp.Compare(merged[i - 1]->largest.Encode(),
                           merged[i]->smallest.Encode()) >= 0) {
            std::fprintf(stderr, "lsmkv: overlapping ranges in level %d\n",
                         level);
            assert(false);
          }
        }
      }
      v->files_[level] = std::move(merged);
    }
  }

 private:
  struct LevelState {
    std::set<uint64_t> deleted;
    std::map<uint64_t, FileRef> added;
  };

  VersionSet* vset_;
  const Version* base_;
  LevelState levels_[config::kNumLevels];
};

// ---- VersionSet -------------------------------------------------------------------

VersionSet::VersionSet(std::string dbname, const Options* options,
                       std::shared_ptr<TableCache> table_cache,
                       const InternalKeyComparator* icmp)
    : env_(options->env),
      dbname_(std::move(dbname)),
      options_(options),
      table_cache_(std::move(table_cache)),
      icmp_(*icmp) {
  AppendVersion(std::make_shared<Version>(icmp_, table_cache_));
}

VersionSet::~VersionSet() = default;

void VersionSet::AppendVersion(std::shared_ptr<Version> v) {
  current_ = std::move(v);
  versions_.push_back(current_);
  versions_.remove_if([](const std::weak_ptr<Version>& w) { return w.expired(); });
}

uint64_t VersionSet::MaxBytesForLevel(int level) const {
  // L1 holds max_bytes_for_level_base; each deeper level is `multiplier`
  // times larger. The geometric growth is what bounds write amplification
  // to about multiplier * number_of_levels.
  double result = static_cast<double>(options_->max_bytes_for_level_base);
  while (level > 1) {
    result *= options_->max_bytes_for_level_multiplier;
    level--;
  }
  return static_cast<uint64_t>(result);
}

void VersionSet::Finalize(Version* v) const {
  int best_level = -1;
  double best_score = -1;
  for (int level = 0; level < config::kNumLevels - 1; level++) {
    double score;
    if (level == 0) {
      // L0 is scored by file count, not bytes: every L0 file must be
      // probed by every read, so the count is what hurts.
      score = static_cast<double>(v->files_[0].size()) /
              static_cast<double>(options_->l0_compaction_trigger);
    } else {
      score = static_cast<double>(v->NumLevelBytes(level)) /
              static_cast<double>(MaxBytesForLevel(level));
    }
    if (score > best_score) {
      best_level = level;
      best_score = score;
    }
  }
  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}

Status VersionSet::WriteSnapshot(log::Writer* log) {
  VersionEdit edit;
  edit.SetComparatorName(icmp_.user_comparator()->Name());
  for (int level = 0; level < config::kNumLevels; level++) {
    if (!compact_pointer_[level].empty()) {
      InternalKey key;
      key.DecodeFrom(compact_pointer_[level]);
      edit.SetCompactPointer(level, key);
    }
  }
  for (int level = 0; level < config::kNumLevels; level++) {
    for (const auto& f : current_->files_[level]) {
      edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
    }
  }
  std::string record;
  edit.EncodeTo(&record);
  return log->AddRecord(record);
}

Status VersionSet::LogAndApply(VersionEdit* edit,
                               std::unique_lock<std::mutex>* lock) {
  if (edit->has_log_number_) {
    assert(edit->log_number_ >= log_number_);
    assert(edit->log_number_ < next_file_number_);
  } else {
    edit->SetLogNumber(log_number_);
  }
  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(LastSequence());

  auto v = std::make_shared<Version>(icmp_, table_cache_);
  {
    Builder builder(this, current_.get());
    builder.Apply(edit);
    builder.SaveTo(v.get());
  }
  Finalize(v.get());

  // First edit since Open: start a fresh MANIFEST holding a full snapshot,
  // so the MANIFEST never grows without bound across restarts.
  std::string new_manifest_file;
  Status s;
  if (descriptor_log_ == nullptr) {
    new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);
    s = env_->NewWritableFile(new_manifest_file, &descriptor_file_);
    if (s.ok()) {
      descriptor_log_ = std::make_unique<log::Writer>(descriptor_file_.get());
      s = WriteSnapshot(descriptor_log_.get());
    }
  }

  // The fsync below can take milliseconds; do not hold the DB mutex (and
  // therefore stall every writer) while it runs. Safe because only one
  // thread ever calls LogAndApply at a time.
  lock->unlock();
  if (s.ok()) {
    std::string record;
    edit->EncodeTo(&record);
    s = descriptor_log_->AddRecord(record);
    if (s.ok()) {
      const uint64_t t0 = env_->NowNanos();
      s = descriptor_file_->Sync();
      if (manifest_sync_ != nullptr) manifest_sync_->Record(env_->NowNanos() - t0);
    }
  }
  // Only after the new MANIFEST is durable does CURRENT switch to it.
  if (s.ok() && !new_manifest_file.empty()) {
    s = SetCurrentFile(env_, dbname_, manifest_file_number_);
  }
  lock->lock();

  if (s.ok()) {
    AppendVersion(std::move(v));
    log_number_ = edit->log_number_;
  } else if (!new_manifest_file.empty()) {
    descriptor_log_.reset();
    descriptor_file_.reset();
    (void)env_->RemoveFile(new_manifest_file);
  }
  return s;
}

Status VersionSet::Recover() {
  std::string current;
  Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
  if (!s.ok()) return s;
  if (current.empty() || current.back() != '\n') {
    return Status::Corruption("CURRENT file does not end with newline");
  }
  current.resize(current.size() - 1);

  const std::string dscname = dbname_ + "/" + current;
  std::unique_ptr<SequentialFile> file;
  s = env_->NewSequentialFile(dscname, &file);
  if (!s.ok()) {
    if (s.IsNotFound()) {
      return Status::Corruption("CURRENT points to a non-existent file",
                                s.ToString());
    }
    return s;
  }

  bool have_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;
  Builder builder(this, current_.get());

  {
    ManifestReporter reporter;
    log::Reader reader(file.get(), &reporter, /*checksum=*/true);
    Slice record;
    std::string scratch;
    while (s.ok() && reader.ReadRecord(&record, &scratch)) {
      if (!reporter.status.ok()) {
        // A valid edit after damaged bytes: the damage is not a torn tail.
        s = Status::Corruption("MANIFEST corrupted before its last record",
                               reporter.status.ToString());
        break;
      }
      VersionEdit edit;
      s = edit.DecodeFrom(record);
      if (s.ok() && edit.has_comparator_ &&
          edit.comparator_ != icmp_.user_comparator()->Name()) {
        s = Status::InvalidArgument(
            edit.comparator_ + " does not match existing comparator ",
            icmp_.user_comparator()->Name());
      }
      if (s.ok()) builder.Apply(&edit);
      if (edit.has_log_number_) {
        log_number = edit.log_number_;
        have_log_number = true;
      }
      if (edit.has_next_file_number_) {
        next_file = edit.next_file_number_;
        have_next_file = true;
      }
      if (edit.has_last_sequence_) {
        last_sequence = edit.last_sequence_;
        have_last_sequence = true;
      }
    }
    // Damage with nothing valid after it is the record being appended when
    // the process died. Its effects (new files) were never published and
    // its inputs never deleted, so ignoring it is safe.
  }
  file.reset();

  if (s.ok()) {
    if (!have_next_file) {
      s = Status::Corruption("no meta-nextfile entry in descriptor");
    } else if (!have_log_number) {
      s = Status::Corruption("no meta-lognumber entry in descriptor");
    } else if (!have_last_sequence) {
      s = Status::Corruption("no last-sequence-number entry in descriptor");
    }
  }

  if (s.ok()) {
    auto v = std::make_shared<Version>(icmp_, table_cache_);
    builder.SaveTo(v.get());
    Finalize(v.get());
    AppendVersion(std::move(v));
    next_file_number_ = next_file;
    MarkFileNumberUsed(log_number);
    manifest_file_number_ = NewFileNumber();
    SetLastSequence(last_sequence);
    log_number_ = log_number;
  }
  return s;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  versions_.remove_if([](const std::weak_ptr<Version>& w) { return w.expired(); });
  for (const auto& weak : versions_) {
    if (auto v = weak.lock()) {
      for (int level = 0; level < config::kNumLevels; level++) {
        for (const auto& f : v->files_[level]) live->insert(f->number);
      }
    }
  }
}

uint64_t VersionSet::EstimatedPendingCompactionBytes() const {
  const Version* v = current_.get();
  uint64_t pending = 0;
  if (v->NumFiles(0) >= options_->l0_compaction_trigger) {
    // An L0 compaction rewrites all of L0 plus the overlapping part of L1.
    pending += v->NumLevelBytes(0) + v->NumLevelBytes(1);
  }
  for (int level = 1; level < config::kNumLevels - 1; level++) {
    const uint64_t bytes = v->NumLevelBytes(level);
    const uint64_t target = MaxBytesForLevel(level);
    if (bytes > target) {
      // Each excess byte is merged with ~multiplier bytes of the next level.
      pending += (bytes - target) *
                 static_cast<uint64_t>(options_->max_bytes_for_level_multiplier + 1);
    }
  }
  return pending;
}

void VersionSet::GetRange(const std::vector<FileRef>& inputs,
                          InternalKey* smallest, InternalKey* largest) const {
  assert(!inputs.empty());
  smallest->Clear();
  largest->Clear();
  for (size_t i = 0; i < inputs.size(); i++) {
    const FileMetaData* f = inputs[i].get();
    if (i == 0) {
      *smallest = f->smallest;
      *largest = f->largest;
    } else {
      if (icmp_.Compare(f->smallest.Encode(), smallest->Encode()) < 0) {
        *smallest = f->smallest;
      }
      if (icmp_.Compare(f->largest.Encode(), largest->Encode()) > 0) {
        *largest = f->largest;
      }
    }
  }
}

void VersionSet::GetRange2(const std::vector<FileRef>& inputs1,
                           const std::vector<FileRef>& inputs2,
                           InternalKey* smallest, InternalKey* largest) const {
  std::vector<FileRef> all = inputs1;
  all.insert(all.end(), inputs2.begin(), inputs2.end());
  GetRange(all, smallest, largest);
}

std::unique_ptr<Compaction> VersionSet::PickCompaction() {
  if (current_->compaction_score_ < 1) return nullptr;
  const int level = current_->compaction_level_;
  assert(level >= 0 && level + 1 < config::kNumLevels);

  auto c = std::make_unique<Compaction>(options_, level, current_);
  if (level == 0) {
    // L0 files usually all overlap one another, so take them all: one
    // merge clears L0 completely instead of leaving stragglers that every
    // read would still have to probe.
    c->inputs_[0] = current_->files_[0];
  } else {
    // Round-robin through the key space: the first file after the key
    // where the last compaction at this level stopped.
    for (const auto& f : current_->files_[level]) {
      if (compact_pointer_[level].empty() ||
          icmp_.Compare(f->largest.Encode(), compact_pointer_[level]) > 0) {
        c->inputs_[0].push_back(f);
        break;
      }
    }
    if (c->inputs_[0].empty()) {
      c->inputs_[0].push_back(current_->files_[level][0]);  // wrap around
    }
  }
  SetupOtherInputs(c.get());
  return c;
}

std::unique_ptr<Compaction> VersionSet::CompactLevel(int level) {
  if (level < 0 || level + 1 >= config::kNumLevels) return nullptr;
  if (current_->files_[level].empty()) return nullptr;
  auto c = std::make_unique<Compaction>(options_, level, current_);
  c->inputs_[0] = current_->files_[level];
  SetupOtherInputs(c.get());
  return c;
}

void VersionSet::SetupOtherInputs(Compaction* c) {
  const int level = c->level();
  InternalKey smallest;
  InternalKey largest;
  GetRange(c->inputs_[0], &smallest, &largest);

  current_->GetOverlappingInputs(level + 1, &smallest, &largest,
                                 &c->inputs_[1]);

  InternalKey all_start;
  InternalKey all_limit;
  GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

  if (level + 2 < config::kNumLevels) {
    current_->GetOverlappingInputs(level + 2, &all_start, &all_limit,
                                   &c->grandparents_);
  }

  // Next compaction at this level starts after this one's range. Updated
  // immediately (not only when the edit commits) so a failed compaction is
  // not retried on the same range forever.
  compact_pointer_[level] = largest.Encode().ToString();
  c->edit_.SetCompactPointer(level, largest);
}

std::unique_ptr<Iterator> VersionSet::MakeInputIterator(Compaction* c) {
  ReadOptions options;
  options.fill_cache = false;  // compaction reads everything once

  std::vector<std::unique_ptr<Iterator>> list;
  for (int which = 0; which < 2; which++) {
    if (c->inputs_[which].empty()) continue;
    if (c->level() + which == 0) {
      std::vector<FileRef> files = c->inputs_[which];
      std::sort(files.begin(), files.end(), [](const FileRef& a, const FileRef& b) {
        return a->number > b->number;
      });
      for (const auto& f : files) {
        list.push_back(table_cache_->NewIterator(options, f->number, f->file_size));
      }
    } else {
      list.push_back(NewTwoLevelIterator(
          std::make_unique<LevelFileNumIterator>(icmp_, c->inputs_[which]),
          FileOpener(table_cache_, options)));
    }
  }
  return NewMergingIterator(&icmp_, std::move(list));
}

// ---- Compaction -------------------------------------------------------------------

Compaction::Compaction(const Options* options, int level,
                       std::shared_ptr<Version> input_version)
    : level_(level),
      max_output_file_size_(options->max_file_size),
      max_grandparent_overlap_bytes_(10 * options->max_file_size),
      input_version_(std::move(input_version)) {}

uint64_t Compaction::InputBytes() const {
  return TotalFileSize(inputs_[0]) + TotalFileSize(inputs_[1]);
}

bool Compaction::IsTrivialMove() const {
  // Avoid a move if there is lots of overlapping grandparent data:
  // otherwise the move could create a parent file that later requires a
  // very expensive merge.
  return num_input_files(0) == 1 && num_input_files(1) == 0 &&
         TotalFileSize(grandparents_) <= max_grandparent_overlap_bytes_;
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
  for (int which = 0; which < 2; which++) {
    for (const auto& f : inputs_[which]) {
      edit->RemoveFile(level_ + which, f->number);
    }
  }
}

bool Compaction::IsBaseLevelForKey(const Slice& user_key) {
  const Comparator* ucmp = input_version_->icmp_.user_comparator();
  for (int lvl = level_ + 2; lvl < config::kNumLevels; lvl++) {
    const std::vector<FileRef>& files = input_version_->files_[lvl];
    while (level_ptrs_[lvl] < files.size()) {
      const FileMetaData* f = files[level_ptrs_[lvl]].get();
      if (ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
        // We've advanced far enough.
        if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0) {
          return false;  // key falls in this file's range
        }
        break;
      }
      level_ptrs_[lvl]++;
    }
  }
  return true;
}

bool Compaction::ShouldStopBefore(const Slice& internal_key) {
  const InternalKeyComparator& icmp = input_version_->icmp_;
  while (grandparent_index_ < grandparents_.size() &&
         icmp.Compare(internal_key,
                      grandparents_[grandparent_index_]->largest.Encode()) > 0) {
    if (seen_key_) {
      overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
    }
    grandparent_index_++;
  }
  seen_key_ = true;
  if (overlapped_bytes_ > max_grandparent_overlap_bytes_) {
    overlapped_bytes_ = 0;
    return true;
  }
  return false;
}

}  // namespace lsmkv
