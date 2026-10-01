// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "db/db_impl.h"

#include <algorithm>
#include <cstdio>
#include <thread>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/filename.h"
#include "db/table_cache.h"
#include "db/write_batch_internal.h"
#include "lsmkv/cache.h"
#include "lsmkv/comparator.h"
#include "lsmkv/filter_policy.h"
#include "memtable/memtable.h"
#include "table/merger.h"
#include "table/table_builder.h"
#include "wal/log_reader.h"

namespace lsmkv {

// ---- DB (default implementations of the public base class) --------------------

DB::~DB() = default;

Status DB::Put(const WriteOptions& options, const Slice& key,
               const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Status DB::Delete(const WriteOptions& options, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}

// ---- Internal types ---------------------------------------------------------------

// One queued write. Lives on the calling thread's stack.
struct DBImpl::Writer {
  enum State : int { kWaiting = 0, kLeader = 1, kDone = 2 };

  Writer(WriteBatch* b, bool s) : batch(b), sync(s) {}
  WriteBatch* batch;  // nullptr: "switch the memtable" request (Flush)
  bool sync;
  // Set (with release) by the thread that holds mu_ and makes this writer
  // the leader or completes it; read (with acquire) by the waiting writer,
  // which may be spinning without the mutex. `status` and `mem` are written
  // before the release-store, so the acquire-load makes them visible.
  std::atomic<int> state{kWaiting};
  bool blocked = false;  // guarded by mu_: waiter is asleep on cv
  Status status;
  // Memtable this writer's batch belongs in, chosen by the leader under
  // mu_. Raw pointer is safe: the memtable cannot be switched (let alone
  // freed) until pending_inserts_ drains, which includes this writer.
  MemTable* mem = nullptr;
  std::condition_variable cv;
};

namespace {

inline void CpuRelax() {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  _mm_pause();  // tells the core we are spinning: saves power, frees the sibling hyperthread
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

}  // namespace

struct DBImpl::CompactionState {
  explicit CompactionState(Compaction* c) : compaction(c) {}

  struct Output {
    uint64_t number = 0;
    uint64_t file_size = 0;
    InternalKey smallest;
    InternalKey largest;
  };
  Output* current_output() { return &outputs.back(); }

  Compaction* const compaction;
  std::vector<Output> outputs;
  std::unique_ptr<WritableFile> outfile;
  std::unique_ptr<TableBuilder> builder;
  uint64_t total_bytes = 0;
  uint64_t entries_in = 0;
  uint64_t entries_dropped = 0;
};

namespace {

template <class T, class V>
void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = static_cast<T>(maxvalue);
  if (static_cast<V>(*ptr) < minvalue) *ptr = static_cast<T>(minvalue);
}

class LogReporter final : public log::Reader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& s) override {
    if (status.ok()) status = s;
    dropped_bytes += bytes;
  }
  Status status;
  uint64_t dropped_bytes = 0;
};

}  // namespace

Options SanitizeOptions(const std::string& dbname, const Options& src) {
  Options result = src;
  if (result.comparator == nullptr) result.comparator = BytewiseComparator();
  if (result.env == nullptr) result.env = Env::Default();
  ClipToRange(&result.max_open_files, 64 + 10, 50000);
  // Tiny memtables only produce tiny L0 files and constant flushing.
  ClipToRange(&result.write_buffer_size, size_t{64} << 10, size_t{1} << 30);
  ClipToRange(&result.max_file_size, size_t{16} << 10, size_t{1} << 30);
  ClipToRange(&result.block_size, size_t{256}, size_t{4} << 20);
  ClipToRange(&result.block_restart_interval, 1, 1024);
  ClipToRange(&result.memtable_shards, 1, 64);
  ClipToRange(&result.l0_compaction_trigger, 1, 1000);
  if (result.l0_slowdown_writes_trigger < result.l0_compaction_trigger) {
    result.l0_slowdown_writes_trigger = result.l0_compaction_trigger;
  }
  if (result.l0_stop_writes_trigger < result.l0_slowdown_writes_trigger) {
    result.l0_stop_writes_trigger = result.l0_slowdown_writes_trigger;
  }
  ClipToRange(&result.max_bytes_for_level_multiplier, 2, 100);
  if (result.filter_policy == nullptr && result.bloom_bits_per_key > 0) {
    result.filter_policy = NewBloomFilterPolicy(result.bloom_bits_per_key);
  }
  if (result.block_cache == nullptr) {
    result.block_cache = NewLRUCache(result.block_cache_capacity);
  }
  if (result.metrics == nullptr) {
    result.metrics = std::make_shared<MetricsRegistry>();
  }
  if (result.info_log == nullptr) {
    // Keep the previous run's log around for post-mortems.
    (void)result.env->CreateDir(dbname);
    if (result.env->FileExists(InfoLogFileName(dbname))) {
      (void)result.env->RenameFile(InfoLogFileName(dbname),
                                   OldInfoLogFileName(dbname));
    }
    if (!NewJsonFileLogger(result.env, InfoLogFileName(dbname),
                           &result.info_log)
             .ok()) {
      result.info_log = NewNullLogger();
    }
  }
  return result;
}

// ---- Construction / destruction ---------------------------------------------------

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : env_(raw_options.env != nullptr ? raw_options.env : Env::Default()),
      internal_comparator_(raw_options.comparator != nullptr
                               ? raw_options.comparator
                               : BytewiseComparator()),
      options_(SanitizeOptions(dbname, raw_options)),
      dbname_(dbname),
      logger_(options_.info_log),
      metrics_(options_.metrics),
      block_cache_(options_.block_cache) {
  table_options_.comparator = &internal_comparator_;
  if (options_.filter_policy != nullptr) {
    table_options_.filter_policy =
        std::make_shared<InternalFilterPolicy>(options_.filter_policy);
  }
  table_options_.block_cache = block_cache_;
  table_options_.block_size = options_.block_size;
  table_options_.block_restart_interval = options_.block_restart_interval;
  SetupMetrics();
  table_cache_ = std::make_shared<TableCache>(dbname_, env_, table_options_,
                                              options_.max_open_files - 10);
  versions_ = std::make_unique<VersionSet>(dbname_, &options_, table_cache_,
                                           &internal_comparator_);
  versions_->SetManifestSyncHistogram(m_.fsync_manifest);
}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> l(mu_);
    shutting_down_.store(true, std::memory_order_release);
  }
  work_cv_.notify_all();
  done_cv_.notify_all();
  if (bg_thread_.joinable()) bg_thread_.join();

  metric_handles_.clear();
  if (logfile_ != nullptr) (void)logfile_->Close();
  logger_->Log(LogLevel::kInfo, "db_closed", LogFields().Add("db", dbname_));
}

void DBImpl::SetupMetrics() {
  MetricsRegistry& r = *metrics_;
  const char* ops_help = "Operations by type";
  m_.puts = r.GetCounter("lsmkv_ops_total", ops_help, {{"op", "put"}});
  m_.deletes = r.GetCounter("lsmkv_ops_total", ops_help, {{"op", "delete"}});
  m_.gets = r.GetCounter("lsmkv_ops_total", ops_help, {{"op", "get"}});
  m_.iterators = r.GetCounter("lsmkv_ops_total", ops_help, {{"op", "scan"}});
  m_.get_misses = r.GetCounter("lsmkv_get_misses_total",
                               "Point lookups that found nothing");
  m_.write_latency = r.GetHistogram("lsmkv_op_latency_seconds",
                                    "Operation latency", {{"op", "write"}});
  m_.get_latency = r.GetHistogram("lsmkv_op_latency_seconds",
                                  "Operation latency", {{"op", "get"}});
  m_.write_groups = r.GetCounter("lsmkv_wal_group_commits_total",
                                 "WAL records written (one per commit group)");
  m_.write_group_writers = r.GetCounter(
      "lsmkv_wal_group_commit_writers_total",
      "Writers committed; / group commits = mean group size");
  m_.write_queue_wait = r.GetHistogram(
      "lsmkv_write_queue_wait_seconds",
      "Time a writer waits in the commit queue before its group is committed");
  m_.wal_append = r.GetHistogram("lsmkv_wal_append_seconds",
                                 "Leader's WAL append for one group (excl. fsync)");
  const char* fsync_help = "fsync latency by file type";
  m_.fsync_wal_seal = r.GetHistogram("lsmkv_fsync_seconds", fsync_help, {{"file", "wal_seal"}});
  m_.fsync_sst = r.GetHistogram("lsmkv_fsync_seconds", fsync_help, {{"file", "sst"}});
  m_.fsync_manifest = r.GetHistogram("lsmkv_fsync_seconds", fsync_help, {{"file", "manifest"}});
  m_.memtable_switch = r.GetHistogram(
      "lsmkv_memtable_switch_seconds",
      "Memtable+WAL switch, during which every writer waits (incl. WAL seal fsync)");
  m_.wal_bytes = r.GetCounter("lsmkv_wal_bytes_written_total",
                              "Bytes appended to the write-ahead log");
  m_.wal_syncs = r.GetCounter("lsmkv_wal_syncs_total", "WAL fsync calls");
  m_.wal_sync_latency =
      r.GetHistogram("lsmkv_wal_fsync_seconds", "WAL fsync latency");
  m_.user_bytes = r.GetCounter("lsmkv_user_bytes_written_total",
                               "Key+value bytes submitted by clients");
  m_.flushes = r.GetCounter("lsmkv_flushes_total", "Memtable flushes");
  m_.flush_bytes = r.GetCounter("lsmkv_flush_bytes_written_total",
                                "Bytes written by memtable flushes (L0)");
  m_.flush_latency =
      r.GetHistogram("lsmkv_flush_seconds", "Memtable flush duration");
  m_.compactions = r.GetCounter("lsmkv_compactions_total",
                                "Compactions that rewrote data");
  m_.trivial_moves = r.GetCounter("lsmkv_trivial_moves_total",
                                  "Compactions done by moving a file down");
  m_.compaction_latency =
      r.GetHistogram("lsmkv_compaction_seconds", "Compaction duration");
  for (int level = 0; level < config::kNumLevels; level++) {
    const MetricLabels l = {{"level", std::to_string(level)}};
    m_.compaction_read[level] =
        r.GetCounter("lsmkv_compaction_bytes_read_total",
                     "Bytes read by compaction, by input level", l);
    m_.compaction_written[level] =
        r.GetCounter("lsmkv_compaction_bytes_written_total",
                     "Bytes written by compaction, by output level", l);
  }
  const char* stall_help = "Write stalls by cause";
  m_.stalls_memtable = r.GetCounter("lsmkv_write_stalls_total", stall_help,
                                    {{"reason", "memtable_full"}});
  m_.stalls_l0_stop = r.GetCounter("lsmkv_write_stalls_total", stall_help,
                                   {{"reason", "l0_stop"}});
  m_.stalls_l0_slowdown = r.GetCounter("lsmkv_write_stalls_total", stall_help,
                                       {{"reason", "l0_slowdown"}});
  m_.stall_micros = r.GetCounter("lsmkv_write_stall_micros_total",
                                 "Microseconds writers spent stalled");
  m_.bg_errors = r.GetCounter("lsmkv_background_errors_total",
                              "Fatal background errors (DB became read-only)");
  m_.recovery_wal_bytes = r.GetGauge("lsmkv_recovery_wal_bytes",
                                     "WAL bytes replayed by the last Open()");
  m_.recovery_records = r.GetGauge("lsmkv_recovery_records",
                                   "WAL records replayed by the last Open()");
  m_.recovery_micros = r.GetGauge("lsmkv_recovery_seconds",
                                  "Wall-clock duration of the last Open()");

  TableReadCounters& tc = table_options_.counters;
  tc.bloom_checks = r.GetCounter("lsmkv_bloom_checks_total",
                                 "SSTable Bloom filter probes");
  tc.bloom_negatives =
      r.GetCounter("lsmkv_bloom_useful_total",
                   "Bloom probes that ruled a table out (no block read)");
  tc.cache_hits = r.GetCounter("lsmkv_block_cache_hits_total",
                               "Data blocks served from the block cache");
  tc.cache_misses = r.GetCounter("lsmkv_block_cache_misses_total",
                                 "Data block cache misses");
  tc.block_reads = r.GetCounter("lsmkv_sst_block_reads_total",
                                "Data blocks read from SSTable files");
  tc.block_read_bytes = r.GetCounter("lsmkv_sst_read_bytes_total",
                                     "Bytes read from SSTable data blocks");

  // Scrape-time gauges. They read only atomics or the SuperVersion, never
  // mu_, so a scrape can never deadlock against the write path.
  auto add = [&](const std::string& name, const std::string& help,
                 const MetricLabels& labels, std::function<double()> fn) {
    metric_handles_.push_back(
        r.RegisterCallbackGauge(name, help, labels, std::move(fn)));
  };
  add("lsmkv_memtable_bytes", "Active memtable arena bytes", {}, [this] {
    auto sv = GetSuperVersion();
    return sv ? static_cast<double>(sv->mem->ApproximateMemoryUsage()) : 0.0;
  });
  add("lsmkv_immutable_memtables", "Memtables waiting to be flushed", {},
      [this] { return has_imm_.load() ? 1.0 : 0.0; });
  add("lsmkv_compaction_pending_bytes",
      "Estimated bytes compaction must rewrite (compaction backlog)", {},
      [this] { return static_cast<double>(pending_compaction_bytes_.load()); });
  for (int level = 0; level < config::kNumLevels; level++) {
    const MetricLabels l = {{"level", std::to_string(level)}};
    add("lsmkv_level_files", "Live SSTables per level", l, [this, level] {
      return static_cast<double>(level_files_[level].load());
    });
    add("lsmkv_level_bytes", "Live SSTable bytes per level", l, [this, level] {
      return static_cast<double>(level_bytes_[level].load());
    });
  }
  add("lsmkv_write_amplification",
      "(WAL + flush + compaction bytes written) / user bytes written", {},
      [this] {
        const double user = static_cast<double>(user_bytes_written_.load());
        if (user == 0) return 0.0;
        return static_cast<double>(wal_bytes_written_.load() +
                                   flush_bytes_written_.load() +
                                   compaction_bytes_written_.load()) /
               user;
      });
  add("lsmkv_block_cache_usage_bytes", "Block cache charge in use", {},
      [this] { return static_cast<double>(block_cache_->TotalCharge()); });
  add("lsmkv_block_cache_hit_ratio", "Block cache hits / lookups", {}, [this] {
    const double h = static_cast<double>(table_options_.counters.cache_hits->Value());
    const double m = static_cast<double>(table_options_.counters.cache_misses->Value());
    return (h + m) == 0 ? 0.0 : h / (h + m);
  });
}

void DBImpl::StartBackgroundThread() {
  bg_thread_ = std::thread([this] { BackgroundThreadMain(); });
}

// ---- Open & recovery ----------------------------------------------------------------

Status DB::Open(const Options& options, const std::string& dbname,
                std::unique_ptr<DB>* dbptr) {
  dbptr->reset();
  auto impl = std::make_unique<DBImpl>(options, dbname);
  const uint64_t start = impl->env_->NowMicros();

  std::unique_lock<std::mutex> lock(impl->mu_);
  VersionEdit edit;
  Status s = impl->Recover(&edit, &lock);
  if (s.ok() && impl->mem_ == nullptr) {
    // The last WAL was not reused: start a fresh one.
    const uint64_t new_log_number = impl->versions_->NewFileNumber();
    std::unique_ptr<WritableFile> lfile;
    s = impl->env_->NewWritableFile(LogFileName(dbname, new_log_number), &lfile);
    if (s.ok()) {
      impl->logfile_ = std::move(lfile);
      impl->logfile_number_ = new_log_number;
      impl->log_ = std::make_unique<log::Writer>(impl->logfile_.get());
      impl->mem_ = std::make_shared<MemTable>(impl->internal_comparator_,
                                              impl->options_.memtable_shards);
    }
  }
  if (s.ok()) {
    // WALs older than the active one are fully captured in SSTables now.
    edit.SetLogNumber(impl->logfile_number_);
    s = impl->versions_->LogAndApply(&edit, &lock);
  }
  if (s.ok()) {
    impl->InstallSuperVersion();
    impl->RemoveObsoleteFiles(&lock);
    impl->StartBackgroundThread();
    impl->work_cv_.notify_one();

    const uint64_t micros = impl->env_->NowMicros() - start;
    impl->m_.recovery_micros->Set(static_cast<double>(micros) / 1e6);
    impl->m_.recovery_wal_bytes->Set(static_cast<double>(impl->recovered_wal_bytes_));
    impl->m_.recovery_records->Set(static_cast<double>(impl->recovered_records_));
    impl->logger_->Log(
        LogLevel::kInfo, "db_opened",
        LogFields()
            .Add("db", dbname)
            .Add("open_micros", micros)
            .Add("wal_bytes_replayed", impl->recovered_wal_bytes_)
            .Add("records_replayed", impl->recovered_records_)
            .Add("last_sequence", impl->versions_->LastSequence())
            .Add("memtable_shards", impl->options_.memtable_shards));
  } else {
    impl->logger_->Log(LogLevel::kError, "db_open_failed",
                       LogFields().Add("db", dbname).Add("status", s.ToString()));
  }
  lock.unlock();
  if (s.ok()) *dbptr = std::move(impl);
  return s;
}

Status DBImpl::NewDB() {
  VersionEdit new_db;
  new_db.SetComparatorName(options_.comparator->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(manifest, &file);
  if (!s.ok()) return s;
  {
    log::Writer log(file.get());
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) s = file->Sync();
    if (s.ok()) s = file->Close();
  }
  file.reset();
  if (s.ok()) {
    s = SetCurrentFile(env_, dbname_, 1);
  } else {
    (void)env_->RemoveFile(manifest);
  }
  return s;
}

Status DBImpl::Recover(VersionEdit* edit, std::unique_lock<std::mutex>* lock) {
  (void)env_->CreateDir(dbname_);
  Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) return s;

  if (!env_->FileExists(CurrentFileName(dbname_))) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument(dbname_,
                                     "does not exist (create_if_missing is false)");
    }
    s = NewDB();
    if (!s.ok()) return s;
  } else if (options_.error_if_exists) {
    return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
  }

  s = versions_->Recover();
  if (!s.ok()) return s;

  // Every SSTable the MANIFEST references must be present. A missing one
  // means the directory was damaged; refuse to serve a silently partial DB.
  std::vector<std::string> filenames;
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) return s;
  std::set<uint64_t> expected;
  versions_->AddLiveFiles(&expected);
  const uint64_t min_log = versions_->LogNumber();
  std::vector<uint64_t> logs;
  for (const std::string& name : filenames) {
    uint64_t number;
    FileType type;
    if (ParseFileName(name, &number, &type)) {
      if (type == FileType::kTableFile) expected.erase(number);
      if (type == FileType::kLogFile && number >= min_log) logs.push_back(number);
    }
  }
  if (!expected.empty()) {
    return Status::Corruption(
        std::to_string(expected.size()) + " missing files; e.g.",
        TableFileName(dbname_, *expected.begin()));
  }

  // Replay WALs oldest first. Sequence numbers make replay idempotent with
  // respect to order inside the memtable.
  std::sort(logs.begin(), logs.end());
  SequenceNumber max_sequence = 0;
  bool stream_damaged = false;
  for (size_t i = 0; i < logs.size(); i++) {
    s = RecoverLogFile(logs[i], i + 1 == logs.size(), edit, &max_sequence,
                       &stream_damaged, lock);
    if (!s.ok()) return s;
    // The previous incarnation may not have recorded this number in the
    // MANIFEST; make sure we never reuse it.
    versions_->MarkFileNumberUsed(logs[i]);
  }
  if (versions_->LastSequence() < max_sequence) {
    versions_->SetLastSequence(max_sequence);
  }
  return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t log_number, bool last_log,
                              VersionEdit* edit, SequenceNumber* max_sequence,
                              bool* stream_damaged,
                              std::unique_lock<std::mutex>* lock) {
  const std::string fname = LogFileName(dbname_, log_number);
  std::unique_ptr<SequentialFile> file;
  Status s = env_->NewSequentialFile(fname, &file);
  if (!s.ok()) return s;
  uint64_t file_size = 0;
  (void)env_->GetFileSize(fname, &file_size);
  recovered_wal_bytes_ += file_size;

  LogReporter reporter;
  log::Reader reader(file.get(), &reporter, /*checksum=*/true);
  const WalRecoveryMode mode = options_.wal_recovery_mode;

  std::string scratch;
  Slice record;
  WriteBatch batch;
  std::shared_ptr<MemTable> mem;
  int flushes = 0;
  uint64_t records = 0;
  uint64_t applied_end = 0;  // end offset of the last record we applied
  bool stopped_early = false;

  while (reader.ReadRecord(&record, &scratch)) {
    if (*stream_damaged || !reporter.status.ok()) {
      // A complete, checksum-valid record *after* damage. A torn write can
      // only ever be the last thing in the WAL stream, so this is genuine
      // corruption, and skipping past it would silently drop acknowledged
      // writes.
      if (mode == WalRecoveryMode::kPointInTimeRecovery) {
        stopped_early = true;
        break;
      }
      return Status::Corruption(
          "WAL " + fname + ": damaged record followed by valid records",
          reporter.status.ok() ? Slice("damage in an earlier WAL")
                               : Slice(reporter.status.ToString()));
    }
    if (record.size() < WriteBatchInternal::kHeader) {
      reporter.Corruption(record.size(),
                          Status::Corruption("log record too small"));
      continue;
    }
    WriteBatchInternal::SetContents(&batch, record);
    if (mem == nullptr) {
      mem = std::make_shared<MemTable>(internal_comparator_,
                                       options_.memtable_shards);
    }
    s = WriteBatchInternal::InsertInto(&batch, mem.get());
    if (!s.ok()) return s;
    records++;
    applied_end = reader.LastRecordEndOffset();
    const SequenceNumber last_seq = WriteBatchInternal::Sequence(&batch) +
                                    WriteBatchInternal::Count(&batch) - 1;
    if (last_seq > *max_sequence) *max_sequence = last_seq;

    if (mem->ApproximateDataUsage() > options_.write_buffer_size) {
      flushes++;
      s = WriteLevel0Table(mem.get(), edit, lock);
      mem.reset();
      if (!s.ok()) return s;
    }
  }
  recovered_records_ += records;

  const bool damaged = !reporter.status.ok() || reader.truncated_tail();
  if (damaged) {
    if (mode == WalRecoveryMode::kAbsoluteConsistency) {
      return Status::Corruption(
          "WAL " + fname + " is damaged (kAbsoluteConsistency)",
          reporter.status.ok() ? Slice("torn final record")
                               : Slice(reporter.status.ToString()));
    }
    *stream_damaged = true;
    logger_->Log(LogLevel::kWarn, "wal_damage_tolerated",
                 LogFields()
                     .Add("file", fname)
                     .Add("reason", reporter.status.ok()
                                        ? std::string("torn final record")
                                        : reporter.status.ToString())
                     .Add("bytes_dropped", reporter.dropped_bytes)
                     .Add("valid_prefix_bytes", applied_end)
                     .Add("file_bytes", file_size));
  }
  file.reset();

  // Cut the damaged tail off the newest WAL. If we did not, new records
  // would be appended *after* the garbage, and the next recovery would
  // (correctly) see "damage followed by valid records" and refuse to open.
  if (last_log && (damaged || stopped_early) && applied_end < file_size) {
    s = env_->TruncateFile(fname, applied_end);
    if (!s.ok()) return s;
    logger_->Log(LogLevel::kWarn, "wal_tail_truncated",
                 LogFields()
                     .Add("file", fname)
                     .Add("from_bytes", file_size)
                     .Add("to_bytes", applied_end));
    file_size = applied_end;
  }

  // Fast restart: keep appending to the newest WAL instead of flushing its
  // contents to an SSTable, provided nothing from it was flushed already.
  if (last_log && flushes == 0) {
    std::unique_ptr<WritableFile> lfile;
    if (env_->NewAppendableFile(fname, &lfile).ok()) {
      logfile_ = std::move(lfile);
      logfile_number_ = log_number;
      log_ = std::make_unique<log::Writer>(logfile_.get(), file_size);
      mem_ = mem != nullptr ? std::move(mem)
                            : std::make_shared<MemTable>(
                                  internal_comparator_, options_.memtable_shards);
      return Status::OK();
    }
  }

  if (mem != nullptr) s = WriteLevel0Table(mem.get(), edit, lock);
  return s;
}

// ---- SuperVersion -------------------------------------------------------------------

std::shared_ptr<const DBImpl::SuperVersion> DBImpl::GetSuperVersion() const {
  std::shared_lock<std::shared_mutex> l(sv_mu_);
  return super_version_;
}

void DBImpl::InstallSuperVersion() {
  auto sv = std::make_shared<SuperVersion>();
  sv->mem = mem_;
  sv->imm = imm_;
  sv->current = versions_->current();
  std::shared_ptr<const SuperVersion> old;
  {
    std::unique_lock<std::shared_mutex> l(sv_mu_);
    old = std::move(super_version_);
    super_version_ = std::move(sv);
  }
  UpdateLevelGauges();
  // `old` is released here, outside sv_mu_: dropping the last reference to
  // a flushed memtable frees megabytes of arena, which must not happen
  // while readers are locked out.
}

void DBImpl::UpdateLevelGauges() {
  const std::shared_ptr<Version> v = versions_->current();
  for (int level = 0; level < config::kNumLevels; level++) {
    level_files_[level].store(static_cast<uint64_t>(v->NumFiles(level)));
    level_bytes_[level].store(v->NumLevelBytes(level));
  }
  pending_compaction_bytes_.store(versions_->EstimatedPendingCompactionBytes());
}

// ---- Write path -------------------------------------------------------------------------

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  const bool timed = options_.enable_latency_metrics && updates != nullptr;
  const uint64_t start = timed ? env_->NowNanos() : 0;

  Writer w(updates, options.sync);
  {
    std::lock_guard<std::mutex> l(mu_);
    writers_.push_back(&w);
    if (writers_.front() == &w) w.state.store(Writer::kLeader, std::memory_order_relaxed);
  }
  AwaitCommitOrLeadership(&w);
  if (timed) m_.write_queue_wait->Record(env_->NowNanos() - start);
  if (w.state.load(std::memory_order_acquire) == Writer::kDone) {
    // A leader already committed our batch to the WAL on our behalf.
    Status s = InsertIntoMemTable(&w);
    if (timed) m_.write_latency->Record(env_->NowNanos() - start);
    return s;
  }

  // We are the leader of a new commit group.
  std::unique_lock<std::mutex> lock(mu_);
  Status status = MakeRoomForWrite(&lock, updates == nullptr);
  Writer* last_writer = &w;
  int64_t group_size = 0;
  if (status.ok() && updates != nullptr) {
    WriteBatch* group = BuildBatchGroup(&last_writer);

    // Number every batch in queue order and bind it to the current
    // memtable. Sequence numbers are what make the parallel, unordered
    // memtable inserts below produce a deterministic result.
    const SequenceNumber first_seq = versions_->LastSequence() + 1;
    SequenceNumber seq = first_seq;
    bool sync = false;
    for (Writer* x : writers_) {
      WriteBatchInternal::SetSequence(x->batch, seq);
      seq += WriteBatchInternal::Count(x->batch);
      x->mem = mem_.get();
      sync = sync || x->sync;
      group_size++;
      if (x == last_writer) break;
    }
    if (group != updates) WriteBatchInternal::SetSequence(group, first_seq);
    const SequenceNumber last_sequence = seq - 1;

    // WAL append (and fsync) with no lock held. Only the leader touches
    // log_, and there is exactly one leader at a time.
    lock.unlock();
    const uint64_t before = log_->bytes_written();
    const uint64_t append_start = timed ? env_->NowNanos() : 0;
    status = log_->AddRecord(WriteBatchInternal::Contents(group));
    if (timed) m_.wal_append->Record(env_->NowNanos() - append_start);
    const uint64_t wal_delta = log_->bytes_written() - before;
    if (status.ok() && sync) {
      const uint64_t t0 = env_->NowNanos();
      status = logfile_->Sync();
      m_.wal_sync_latency->Record(env_->NowNanos() - t0);
      m_.wal_syncs->Inc();
    }
    wal_bytes_written_.fetch_add(wal_delta, std::memory_order_relaxed);
    m_.wal_bytes->Inc(wal_delta);
    m_.write_groups->Inc();
    m_.write_group_writers->Inc(static_cast<uint64_t>(group_size));
    lock.lock();

    if (status.ok()) {
      versions_->SetLastSequence(last_sequence);
    } else {
      // We cannot know how much of the record reached the disk, and after
      // a failed fsync the kernel may have dropped the dirty pages. The
      // only safe response is to stop accepting writes.
      RecordBackgroundError(status);
    }
    if (group == &tmp_batch_) tmp_batch_.Clear();
  }

  // Release the group: followers insert into the memtable in parallel.
  if (status.ok() && updates != nullptr) {
    pending_inserts_.fetch_add(group_size, std::memory_order_seq_cst);
  }
  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      SetWriterState(ready, Writer::kDone);
    }
    if (ready == last_writer) break;
  }
  // Hand leadership to the next queued writer, if any.
  if (!writers_.empty()) SetWriterState(writers_.front(), Writer::kLeader);
  lock.unlock();

  w.status = status;
  Status s = InsertIntoMemTable(&w);
  if (timed) m_.write_latency->Record(env_->NowNanos() - start);
  return s;
}

void DBImpl::SetWriterState(Writer* w, int state) {
  // REQUIRES: mu_ held. Holding mu_ is what makes the hand-off lossless: a
  // waiter only sets `blocked` and goes to sleep under mu_, after checking
  // `state` under mu_, so either it sees this store or we see `blocked`.
  //
  // Read `blocked` *before* publishing the state. A writer that is spinning
  // (not blocked) may observe the store, return from Write() and destroy
  // its stack-allocated Writer immediately -- so after the store we must
  // not touch *w at all unless it is asleep. An asleep writer cannot run
  // until it re-acquires mu_, which we hold, so notifying it is safe.
  const bool blocked = w->blocked;
  w->state.store(state, std::memory_order_release);
  if (blocked) w->cv.notify_one();
}

void DBImpl::AwaitCommitOrLeadership(Writer* w) {
  auto settled = [w] {
    return w->state.load(std::memory_order_acquire) != Writer::kWaiting;
  };
  if (settled()) return;

  if (options_.adaptive_write_wait) {
    // Phase 1: spin. A commit group is one WAL append -- a few
    // microseconds -- while putting a thread to sleep and waking it again
    // costs tens of microseconds and sits on the critical path of the next
    // leader. ~200 pause instructions is on the order of 1-5 us.
    for (int i = 0; i < 200; i++) {
      if (settled()) return;
      CpuRelax();
    }
    // Phase 2: yield the core but stay runnable, for up to 100 us. Keeps
    // the engine polite when there are more writers than cores.
    const uint64_t deadline = env_->NowNanos() + 100000;
    while (env_->NowNanos() < deadline) {
      if (settled()) return;
      std::this_thread::yield();
    }
  }
  // Phase 3 (and the baseline behaviour): block on this writer's condvar.
  std::unique_lock<std::mutex> lock(mu_);
  w->blocked = true;
  while (!settled()) w->cv.wait(lock);
  w->blocked = false;
}

Status DBImpl::InsertIntoMemTable(Writer* w) {
  if (!w->status.ok() || w->batch == nullptr) return w->status;
  WriteBatchInternal::InsertStats st;
  Status s = WriteBatchInternal::InsertInto(w->batch, w->mem, &st);
  m_.puts->Inc(st.puts);
  m_.deletes->Inc(st.deletes);
  m_.user_bytes->Inc(st.payload_bytes);
  user_bytes_written_.fetch_add(st.payload_bytes, std::memory_order_relaxed);

  // Drop our claim on the memtable. seq_cst pairs with WaitForPendingInserts:
  // either we observe switch_waiting_ == true and wake the switcher, or the
  // switcher observes our decrement. (Dekker-style; both sides seq_cst.)
  if (pending_inserts_.fetch_sub(1, std::memory_order_seq_cst) == 1 &&
      switch_waiting_.load(std::memory_order_seq_cst)) {
    std::lock_guard<std::mutex> l(mu_);
    inserts_cv_.notify_all();
  }
  return s;
}

void DBImpl::WaitForPendingInserts(std::unique_lock<std::mutex>* lock) {
  switch_waiting_.store(true, std::memory_order_seq_cst);
  inserts_cv_.wait(*lock, [this] {
    return pending_inserts_.load(std::memory_order_seq_cst) == 0;
  });
  switch_waiting_.store(false, std::memory_order_seq_cst);
}

WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  size_t size = WriteBatchInternal::ByteSize(first->batch);

  // Bound the group so a small write is not held up behind megabytes of
  // other writers' data.
  size_t max_size = options_.max_write_group_bytes;
  if (size <= (128 << 10)) max_size = size + (128 << 10);

  *last_writer = first;
  for (auto it = writers_.begin() + 1; it != writers_.end(); ++it) {
    Writer* w = *it;
    if (w->batch == nullptr) break;  // a Flush request: its own group
    size += WriteBatchInternal::ByteSize(w->batch);
    if (size > max_size) break;
    if (result == first->batch) {
      // Switch to the scratch batch so the caller's batch is not modified.
      result = &tmp_batch_;
      tmp_batch_.Clear();
      WriteBatchInternal::Append(result, first->batch);
    }
    WriteBatchInternal::Append(result, w->batch);
    *last_writer = w;
  }
  return result;
}

void DBImpl::RecordStall(const char* reason, uint64_t start_micros) {
  const uint64_t micros = env_->NowMicros() - start_micros;
  stall_count_.fetch_add(1, std::memory_order_relaxed);
  stall_micros_.fetch_add(micros, std::memory_order_relaxed);
  m_.stall_micros->Inc(micros);
  const std::string r(reason);
  if (r == "memtable_full") {
    m_.stalls_memtable->Inc();
  } else if (r == "l0_stop") {
    m_.stalls_l0_stop->Inc();
  } else {
    m_.stalls_l0_slowdown->Inc();
    return;  // 1 ms slowdowns are frequent by design; do not flood the log
  }
  logger_->Log(LogLevel::kWarn, "write_stall",
               LogFields()
                   .Add("reason", r)
                   .Add("micros", micros)
                   .Add("l0_files", versions_->NumLevelFiles(0)));
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>* lock,
                                bool force) {
  bool allow_delay = !force;
  Status s;
  const bool compactions_enabled = !options_.disable_auto_compactions;
  while (true) {
    if (!bg_error_.ok()) {
      s = bg_error_;
      break;
    }
    const int l0_files = versions_->NumLevelFiles(0);
    if (allow_delay && compactions_enabled &&
        l0_files >= options_.l0_slowdown_writes_trigger) {
      // Soft limit: delay this one write by 1 ms instead of letting L0 hit
      // the hard limit and stopping every writer for seconds. Spreads the
      // latency cost and hands CPU to the compaction thread.
      const uint64_t t0 = env_->NowMicros();
      lock->unlock();
      env_->SleepForMicroseconds(1000);
      lock->lock();
      RecordStall("l0_slowdown", t0);
      allow_delay = false;  // at most one delay per write
    } else if (!force && (mem_->NumEntries() == 0 ||
                          mem_->ApproximateDataUsage() <=
                              options_.write_buffer_size)) {
      break;  // there is room in the current memtable
    } else if (force && mem_->NumEntries() == 0) {
      break;  // nothing to flush
    } else if (imm_ != nullptr) {
      // The previous memtable is still being flushed. Hard stall.
      const uint64_t t0 = env_->NowMicros();
      done_cv_.wait(*lock);
      RecordStall("memtable_full", t0);
    } else if (compactions_enabled &&
               l0_files >= options_.l0_stop_writes_trigger) {
      // Too many L0 files: every read would probe them all. Hard stall.
      const uint64_t t0 = env_->NowMicros();
      done_cv_.wait(*lock);
      RecordStall("l0_stop", t0);
    } else {
      // Switch to a fresh memtable + WAL; the old pair becomes imm_ and is
      // flushed in the background.
      const uint64_t switch_start = env_->NowNanos();
      WaitForPendingInserts(lock);
      const uint64_t new_log_number = versions_->NewFileNumber();
      std::unique_ptr<WritableFile> lfile;
      lock->unlock();
      // Seal the old WAL with an fsync. Afterwards only the newest WAL can
      // ever hold a torn record -- the property recovery uses to tell a
      // torn tail from real corruption.
      const uint64_t seal_start = env_->NowNanos();
      Status sync_status = logfile_->Sync();
      m_.fsync_wal_seal->Record(env_->NowNanos() - seal_start);
      if (sync_status.ok()) {
        s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
      }
      lock->lock();
      if (!sync_status.ok()) {
        RecordBackgroundError(sync_status);
        s = sync_status;
        break;
      }
      if (!s.ok()) {
        versions_->ReuseFileNumber(new_log_number);
        break;
      }
      (void)logfile_->Close();
      logfile_ = std::move(lfile);
      logfile_number_ = new_log_number;
      log_ = std::make_unique<log::Writer>(logfile_.get());
      imm_ = mem_;
      has_imm_.store(true, std::memory_order_release);
      mem_ = std::make_shared<MemTable>(internal_comparator_,
                                        options_.memtable_shards);
      force = false;  // do not switch again
      InstallSuperVersion();
      work_cv_.notify_one();
      m_.memtable_switch->Record(env_->NowNanos() - switch_start);
    }
  }
  return s;
}

// ---- Read path ------------------------------------------------------------------------

Status DBImpl::Get(const ReadOptions& options, const Slice& key,
                   std::string* value) {
  const bool timed = options_.enable_latency_metrics;
  const uint64_t start = timed ? env_->NowNanos() : 0;

  // One shared-lock acquisition to pin {mem, imm, version}; everything
  // after this runs without any DB-level lock.
  const std::shared_ptr<const SuperVersion> sv = GetSuperVersion();
  const LookupKey lkey(key, kMaxSequenceNumber);
  Status s;
  if (sv->mem->Get(lkey, value, &s)) {
    // Found in the active memtable (value or tombstone).
  } else if (sv->imm != nullptr && sv->imm->Get(lkey, value, &s)) {
    // Found in the memtable being flushed.
  } else {
    s = sv->current->Get(options, lkey, value);
  }

  m_.gets->Inc();
  if (s.IsNotFound()) m_.get_misses->Inc();
  if (timed) m_.get_latency->Record(env_->NowNanos() - start);
  return s;
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) {
  // Read the sequence *before* pinning the SuperVersion: every write with a
  // sequence <= snapshot was committed before this point, so it lives in
  // the pinned memtables or SSTables.
  const SequenceNumber snapshot = versions_->LastSequence();
  std::shared_ptr<const SuperVersion> sv = GetSuperVersion();

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(sv->mem->NewIterator());  // newest source first
  if (sv->imm != nullptr) children.push_back(sv->imm->NewIterator());
  sv->current->AddIterators(options, &children);
  auto internal = NewMergingIterator(&internal_comparator_, std::move(children));
  m_.iterators->Inc();
  return NewDBIterator(options_.comparator, std::move(internal), snapshot,
                       std::move(sv));
}

// ---- Background work ------------------------------------------------------------------

bool DBImpl::HasBackgroundWork() const {
  if (!bg_error_.ok()) return false;
  if (imm_ != nullptr) return true;
  if (manual_compaction_level_ >= 0) return true;
  return !options_.disable_auto_compactions && versions_->NeedsCompaction();
}

void DBImpl::BackgroundThreadMain() {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    work_cv_.wait(lock, [this] {
      return shutting_down_.load(std::memory_order_acquire) || HasBackgroundWork();
    });
    if (shutting_down_.load(std::memory_order_acquire)) break;
    bg_active_ = true;
    if (imm_ != nullptr) {
      BackgroundFlush(&lock);  // flushes always go first: writers wait on them
    } else {
      BackgroundCompaction(&lock);
    }
    bg_active_ = false;
    done_cv_.notify_all();
  }
  done_cv_.notify_all();
}

void DBImpl::BackgroundFlush(std::unique_lock<std::mutex>* lock) {
  std::shared_ptr<MemTable> imm = imm_;
  VersionEdit edit;
  Status s = WriteLevel0Table(imm.get(), &edit, lock);
  if (s.ok()) {
    // The flushed memtable's WAL (and any older one) is now redundant.
    edit.SetLogNumber(logfile_number_);
    s = versions_->LogAndApply(&edit, lock);
  }
  if (s.ok()) {
    imm_.reset();
    has_imm_.store(false, std::memory_order_release);
    InstallSuperVersion();
    m_.flushes->Inc();
    RemoveObsoleteFiles(lock);
  } else {
    RecordBackgroundError(s);
  }
  done_cv_.notify_all();
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                std::unique_lock<std::mutex>* lock) {
  const uint64_t start = env_->NowMicros();
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  std::unique_ptr<Iterator> iter = mem->NewIterator();
  const uint64_t entries = mem->NumEntries();
  const uint64_t mem_bytes = mem->ApproximateMemoryUsage();

  BuildTableStats bstats;
  lock->unlock();
  Status s = BuildTable(dbname_, env_, table_options_, internal_comparator_,
                        table_cache_.get(), iter.get(), &meta, &bstats);
  iter.reset();
  lock->lock();
  pending_outputs_.erase(meta.number);

  if (s.ok() && meta.file_size > 0) {
    edit->AddFile(0, meta.number, meta.file_size, meta.smallest, meta.largest);
  }
  const uint64_t micros = env_->NowMicros() - start;
  LevelStats& st = stats_[0];
  st.micros += micros;
  st.bytes_written += meta.file_size;
  st.count++;
  flush_bytes_written_.fetch_add(meta.file_size, std::memory_order_relaxed);
  m_.flush_bytes->Inc(meta.file_size);
  if (bstats.sync_nanos > 0) m_.fsync_sst->Record(bstats.sync_nanos);
  m_.flush_latency->Record(micros * 1000);
  logger_->Log(s.ok() ? LogLevel::kInfo : LogLevel::kError, "flush_finished",
               LogFields()
                   .Add("file", meta.number)
                   .Add("file_bytes", meta.file_size)
                   .Add("memtable_bytes", mem_bytes)
                   .Add("entries", entries)
                   .Add("entries_written", bstats.entries_out)
                   .Add("micros", micros)
                   .Add("status", s.ToString()));
  return s;
}

void DBImpl::BackgroundCompaction(std::unique_lock<std::mutex>* lock) {
  const bool is_manual = manual_compaction_level_ >= 0;
  std::unique_ptr<Compaction> c =
      is_manual ? versions_->CompactLevel(manual_compaction_level_)
                : versions_->PickCompaction();

  if (c == nullptr) {
    // Nothing to do.
  } else if (!is_manual && c->IsTrivialMove()) {
    // Move the file to the next level by editing metadata only.
    const FileMetaData* f = c->input(0, 0);
    const uint64_t number = f->number;
    const uint64_t size = f->file_size;
    c->edit()->RemoveFile(c->level(), number);
    c->edit()->AddFile(c->level() + 1, number, size, f->smallest, f->largest);
    Status s = versions_->LogAndApply(c->edit(), lock);
    if (s.ok()) {
      InstallSuperVersion();
      m_.trivial_moves->Inc();
      logger_->Log(LogLevel::kInfo, "trivial_move",
                   LogFields()
                       .Add("file", number)
                       .Add("bytes", size)
                       .Add("from_level", c->level())
                       .Add("to_level", c->level() + 1));
    } else {
      RecordBackgroundError(s);
    }
  } else {
    {
      CompactionState compact(c.get());
      Status s = DoCompactionWork(&compact, lock);
      if (!s.ok() && !shutting_down_.load(std::memory_order_acquire)) {
        RecordBackgroundError(s);
      }
      CleanupCompaction(&compact);
    }
    // The Compaction pins its input Version (and so its input files).
    // Release it first, or the files it just merged would survive this
    // garbage-collection pass.
    c.reset();
    RemoveObsoleteFiles(lock);
  }

  if (is_manual) {
    manual_compaction_level_ = -1;
    manual_compaction_done_ = true;
  }
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact,
                                        std::unique_lock<std::mutex>* lock) {
  uint64_t file_number;
  {
    lock->lock();
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
    CompactionState::Output out;
    out.number = file_number;
    compact->outputs.push_back(out);
    lock->unlock();
  }
  Status s = env_->NewWritableFile(TableFileName(dbname_, file_number),
                                   &compact->outfile);
  if (s.ok()) {
    compact->builder =
        std::make_unique<TableBuilder>(table_options_, compact->outfile.get());
  }
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact) {
  const uint64_t output_number = compact->current_output()->number;
  Status s = compact->builder->Finish();
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  compact->builder.reset();

  // Durable and verified readable before the MANIFEST may reference it.
  if (s.ok()) {
    const uint64_t t0 = env_->NowNanos();
    s = compact->outfile->Sync();
    m_.fsync_sst->Record(env_->NowNanos() - t0);
  }
  if (s.ok()) s = compact->outfile->Close();
  compact->outfile.reset();
  if (s.ok()) {
    std::shared_ptr<Table> table;
    s = table_cache_->FindTable(output_number, current_bytes, &table);
  }
  return s;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact,
                                        std::unique_lock<std::mutex>* lock) {
  Compaction* c = compact->compaction;
  c->AddInputDeletions(c->edit());
  const int level = c->level();
  for (const auto& out : compact->outputs) {
    c->edit()->AddFile(level + 1, out.number, out.file_size, out.smallest,
                       out.largest);
  }
  // One MANIFEST record: inputs disappear and outputs appear atomically.
  Status s = versions_->LogAndApply(c->edit(), lock);
  if (s.ok()) InstallSuperVersion();
  return s;
}

void DBImpl::CleanupCompaction(CompactionState* compact) {
  if (compact->builder != nullptr) {
    compact->builder->Abandon();
    compact->builder.reset();
  }
  compact->outfile.reset();
  for (const auto& out : compact->outputs) pending_outputs_.erase(out.number);
}

Status DBImpl::DoCompactionWork(CompactionState* compact,
                                std::unique_lock<std::mutex>* lock) {
  const uint64_t start = env_->NowMicros();
  Compaction* c = compact->compaction;
  const int level = c->level();
  uint64_t input_bytes[2] = {0, 0};
  for (int which = 0; which < 2; which++) {
    for (int i = 0; i < c->num_input_files(which); i++) {
      input_bytes[which] += c->input(which, i)->file_size;
    }
  }
  logger_->Log(LogLevel::kInfo, "compaction_started",
               LogFields()
                   .Add("level", level)
                   .Add("output_level", level + 1)
                   .Add("inputs", c->num_input_files(0))
                   .Add("inputs_next_level", c->num_input_files(1))
                   .Add("input_bytes", input_bytes[0] + input_bytes[1]));

  std::unique_ptr<Iterator> input = versions_->MakeInputIterator(c);
  const Comparator* ucmp = internal_comparator_.user_comparator();

  lock->unlock();
  input->SeekToFirst();
  Status status;
  std::string current_user_key;
  bool has_current_user_key = false;
  while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
    // A pending memtable flush takes priority: writers may be stalled on it.
    if (has_imm_.load(std::memory_order_relaxed)) {
      lock->lock();
      if (imm_ != nullptr) BackgroundFlush(lock);
      lock->unlock();
    }

    const Slice key = input->key();
    if (compact->builder != nullptr && c->ShouldStopBefore(key)) {
      status = FinishCompactionOutputFile(compact);
      if (!status.ok()) break;
    }

    compact->entries_in++;
    ParsedInternalKey ikey;
    if (!ParseInternalKey(key, &ikey)) {
      status = Status::Corruption("corrupted internal key during compaction");
      break;
    }
    bool drop = false;
    if (!has_current_user_key ||
        ucmp->Compare(ikey.user_key, Slice(current_user_key)) != 0) {
      // First (= newest) entry for this user key.
      current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
      has_current_user_key = true;
      // A tombstone with nothing older beneath the output level has done
      // its job: nothing is left for it to shadow.
      if (ikey.type == kTypeDeletion && c->IsBaseLevelForKey(ikey.user_key)) {
        drop = true;
      }
    } else {
      // An older version of a key we just emitted. With no snapshots,
      // nobody can ever read it again.
      drop = true;
    }

    if (drop) {
      compact->entries_dropped++;
    } else {
      if (compact->builder == nullptr) {
        status = OpenCompactionOutputFile(compact, lock);
        if (!status.ok()) break;
      }
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.DecodeFrom(key);
      }
      compact->current_output()->largest.DecodeFrom(key);
      compact->builder->Add(key, input->value());
      if (compact->builder->FileSize() >= c->MaxOutputFileSize()) {
        status = FinishCompactionOutputFile(compact);
        if (!status.ok()) break;
      }
    }
    input->Next();
  }

  if (status.ok() && shutting_down_.load(std::memory_order_acquire)) {
    status = Status::IOError("shutting down during compaction");
  }
  if (status.ok() && compact->builder != nullptr) {
    status = FinishCompactionOutputFile(compact);
  }
  if (status.ok()) status = input->status();
  input.reset();

  const uint64_t micros = env_->NowMicros() - start;
  lock->lock();
  LevelStats& st = stats_[level + 1];
  st.micros += micros;
  st.bytes_read += input_bytes[0] + input_bytes[1];
  st.bytes_written += compact->total_bytes;
  st.count++;
  compaction_bytes_read_.fetch_add(input_bytes[0] + input_bytes[1]);
  compaction_bytes_written_.fetch_add(compact->total_bytes);
  m_.compaction_read[level]->Inc(input_bytes[0]);
  m_.compaction_read[level + 1]->Inc(input_bytes[1]);
  m_.compaction_written[level + 1]->Inc(compact->total_bytes);
  m_.compaction_latency->Record(micros * 1000);

  if (status.ok() && !bg_error_.ok()) status = bg_error_;
  if (status.ok()) status = InstallCompactionResults(compact, lock);
  if (status.ok()) m_.compactions->Inc();

  logger_->Log(status.ok() ? LogLevel::kInfo : LogLevel::kError,
               "compaction_finished",
               LogFields()
                   .Add("level", level)
                   .Add("output_level", level + 1)
                   .Add("bytes_read", input_bytes[0] + input_bytes[1])
                   .Add("bytes_written", compact->total_bytes)
                   .Add("outputs", compact->outputs.size())
                   .Add("entries_in", compact->entries_in)
                   .Add("entries_dropped", compact->entries_dropped)
                   .Add("micros", micros)
                   .Add("status", status.ToString()));
  return status;
}

void DBImpl::RemoveObsoleteFiles(std::unique_lock<std::mutex>* lock) {
  if (!bg_error_.ok()) {
    // After a background error we cannot be sure what the MANIFEST says is
    // live; deleting anything could destroy the only copy of some data.
    return;
  }
  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);
  const uint64_t min_log = versions_->LogNumber();
  const uint64_t manifest = versions_->ManifestFileNumber();

  std::vector<std::string> filenames;
  (void)env_->GetChildren(dbname_, &filenames);
  std::vector<std::string> to_delete;
  for (const std::string& name : filenames) {
    uint64_t number;
    FileType type;
    if (!ParseFileName(name, &number, &type)) continue;
    bool keep = true;
    switch (type) {
      case FileType::kLogFile:
        keep = number >= min_log || number == logfile_number_;
        break;
      case FileType::kDescriptorFile:
        keep = number >= manifest;
        break;
      case FileType::kTableFile:
      case FileType::kTempFile:
        keep = live.count(number) > 0;
        break;
      case FileType::kCurrentFile:
      case FileType::kDBLockFile:
      case FileType::kInfoLogFile:
        keep = true;
        break;
    }
    if (!keep) {
      if (type == FileType::kTableFile) table_cache_->Evict(number);
      to_delete.push_back(name);
    }
  }

  // Unlink without the mutex; the files are unreachable from any Version.
  lock->unlock();
  for (const std::string& name : to_delete) {
    (void)env_->RemoveFile(dbname_ + "/" + name);
  }
  lock->lock();
}

void DBImpl::RecordBackgroundError(const Status& s) {
  if (bg_error_.ok()) {
    bg_error_ = s;
    m_.bg_errors->Inc();
    logger_->Log(LogLevel::kError, "background_error",
                 LogFields().Add("status", s.ToString()));
    done_cv_.notify_all();
  }
}

// ---- Manual control -------------------------------------------------------------------

Status DBImpl::Flush() {
  Status s = Write(WriteOptions(), nullptr);  // forces a memtable switch
  if (!s.ok()) return s;
  std::unique_lock<std::mutex> lock(mu_);
  done_cv_.wait(lock, [this] { return imm_ == nullptr || !bg_error_.ok(); });
  return bg_error_;
}

Status DBImpl::WaitForCompactions() {
  std::unique_lock<std::mutex> lock(mu_);
  work_cv_.notify_one();
  done_cv_.wait(lock, [this] {
    return !bg_error_.ok() || (!bg_active_ && !HasBackgroundWork());
  });
  return bg_error_;
}

Status DBImpl::CompactAll() {
  Status s = Flush();
  if (!s.ok()) return s;
  std::unique_lock<std::mutex> lock(mu_);
  int max_level_with_files = 1;
  {
    const std::shared_ptr<Version> v = versions_->current();
    for (int level = 1; level < config::kNumLevels; level++) {
      if (v->NumFiles(level) > 0) max_level_with_files = level;
    }
  }
  for (int level = 0; level < max_level_with_files; level++) {
    manual_compaction_level_ = level;
    manual_compaction_done_ = false;
    work_cv_.notify_one();
    done_cv_.wait(lock, [this] {
      return manual_compaction_done_ || !bg_error_.ok() ||
             shutting_down_.load(std::memory_order_acquire);
    });
    if (!bg_error_.ok()) return bg_error_;
  }
  lock.unlock();
  return WaitForCompactions();
}

int DBImpl::NumLevelFiles(int level) {
  std::lock_guard<std::mutex> l(mu_);
  return versions_->NumLevelFiles(level);
}

// ---- Introspection ----------------------------------------------------------------------

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();
  Slice in = property;
  const Slice prefix("lsmkv.");
  if (!in.starts_with(prefix)) return false;
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    in.remove_prefix(std::string("num-files-at-level").size());
    const std::string digits = in.ToString();
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
      return false;
    }
    const int level = std::stoi(digits);
    if (level >= config::kNumLevels) return false;
    *value = std::to_string(NumLevelFiles(level));
    return true;
  }
  if (in == Slice("stats")) {
    *value = StatsString();
    return true;
  }
  if (in == Slice("json-stats")) {
    *value = JsonStats();
    return true;
  }
  if (in == Slice("prometheus")) {
    *value = metrics_->ExportPrometheus();
    return true;
  }
  if (in == Slice("write-amplification")) {
    const double user = static_cast<double>(user_bytes_written_.load());
    const double total = static_cast<double>(wal_bytes_written_.load() +
                                             flush_bytes_written_.load() +
                                             compaction_bytes_written_.load());
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", user == 0 ? 0.0 : total / user);
    *value = buf;
    return true;
  }
  if (in == Slice("sstables")) {
    std::lock_guard<std::mutex> l(mu_);
    *value = versions_->current()->DebugString();
    return true;
  }
  if (in == Slice("approximate-memory-usage")) {
    auto sv = GetSuperVersion();
    size_t total = block_cache_->TotalCharge() + sv->mem->ApproximateMemoryUsage();
    if (sv->imm != nullptr) total += sv->imm->ApproximateMemoryUsage();
    *value = std::to_string(total);
    return true;
  }
  return false;
}

std::string DBImpl::StatsString() {
  std::unique_lock<std::mutex> lock(mu_);
  const std::shared_ptr<Version> v = versions_->current();
  std::string out;
  char buf[256];
  out +=
      "                          Compactions\n"
      "Level  Files  Size(MB)  Count  Time(s)  Read(MB)  Write(MB)\n"
      "-----------------------------------------------------------\n";
  for (int level = 0; level < config::kNumLevels; level++) {
    const int files = v->NumFiles(level);
    if (stats_[level].count > 0 || files > 0) {
      std::snprintf(buf, sizeof(buf),
                    "%5d %6d %9.2f %6llu %8.2f %9.2f %10.2f\n", level, files,
                    v->NumLevelBytes(level) / 1048576.0,
                    static_cast<unsigned long long>(stats_[level].count),
                    stats_[level].micros / 1e6,
                    stats_[level].bytes_read / 1048576.0,
                    stats_[level].bytes_written / 1048576.0);
      out += buf;
    }
  }
  const double user = static_cast<double>(user_bytes_written_.load());
  const uint64_t wal = wal_bytes_written_.load();
  const uint64_t flush = flush_bytes_written_.load();
  const uint64_t comp = compaction_bytes_written_.load();
  std::snprintf(buf, sizeof(buf),
                "Write amplification: %.2f  (user %.1f MB; WAL %.1f MB, flush "
                "%.1f MB, compaction %.1f MB)\n",
                user == 0 ? 0.0 : (wal + flush + comp) / user, user / 1048576.0,
                wal / 1048576.0, flush / 1048576.0, comp / 1048576.0);
  out += buf;
  std::snprintf(buf, sizeof(buf), "Write stalls: %llu (%.1f ms total)\n",
                static_cast<unsigned long long>(stall_count_.load()),
                stall_micros_.load() / 1000.0);
  out += buf;
  return out;
}

std::string DBImpl::JsonStats() {
  std::unique_lock<std::mutex> lock(mu_);
  const std::shared_ptr<Version> v = versions_->current();
  std::string out = "{\"levels\":[";
  for (int level = 0; level < config::kNumLevels; level++) {
    if (level > 0) out += ",";
    out += "{\"level\":" + std::to_string(level) +
           ",\"files\":" + std::to_string(v->NumFiles(level)) +
           ",\"bytes\":" + std::to_string(v->NumLevelBytes(level)) +
           ",\"compactions\":" + std::to_string(stats_[level].count) +
           ",\"micros\":" + std::to_string(stats_[level].micros) +
           ",\"bytes_read\":" + std::to_string(stats_[level].bytes_read) +
           ",\"bytes_written\":" + std::to_string(stats_[level].bytes_written) +
           "}";
  }
  const CacheStats cs = block_cache_->GetStats();
  out += "],\"user_bytes\":" + std::to_string(user_bytes_written_.load()) +
         ",\"wal_bytes\":" + std::to_string(wal_bytes_written_.load()) +
         ",\"flush_bytes\":" + std::to_string(flush_bytes_written_.load()) +
         ",\"compaction_bytes_read\":" + std::to_string(compaction_bytes_read_.load()) +
         ",\"compaction_bytes_written\":" +
         std::to_string(compaction_bytes_written_.load()) +
         ",\"stall_count\":" + std::to_string(stall_count_.load()) +
         ",\"stall_micros\":" + std::to_string(stall_micros_.load()) +
         ",\"pending_compaction_bytes\":" +
         std::to_string(versions_->EstimatedPendingCompactionBytes()) +
         ",\"block_cache_hits\":" + std::to_string(cs.hits) +
         ",\"block_cache_misses\":" + std::to_string(cs.misses) +
         ",\"memtable_shards\":" + std::to_string(options_.memtable_shards) + "}";
  return out;
}

// ---- DestroyDB --------------------------------------------------------------------------

Status DestroyDB(const std::string& dbname, const Options& options) {
  Env* env = options.env != nullptr ? options.env : Env::Default();
  std::vector<std::string> filenames;
  Status result = env->GetChildren(dbname, &filenames);
  if (!result.ok()) {
    // Ignore error in case directory does not exist
    return Status::OK();
  }
  std::unique_ptr<FileLock> lock;
  const std::string lockname = LockFileName(dbname);
  result = env->LockFile(lockname, &lock);
  if (result.ok()) {
    for (const std::string& name : filenames) {
      uint64_t number;
      FileType type;
      if (ParseFileName(name, &number, &type) && type != FileType::kDBLockFile) {
        Status del = env->RemoveFile(dbname + "/" + name);
        if (result.ok() && !del.ok()) result = del;
      }
    }
    lock.reset();  // release before deleting the lock file itself
    (void)env->RemoveFile(lockname);
    (void)env->RemoveDir(dbname);
  }
  return result;
}

}  // namespace lsmkv
