// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_DB_DB_IMPL_H_
#define LSMKV_SRC_DB_DB_IMPL_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "db/dbformat.h"
#include "db/version_set.h"
#include "lsmkv/db.h"
#include "lsmkv/env.h"
#include "lsmkv/logger.h"
#include "lsmkv/metrics.h"
#include "table/format.h"
#include "wal/log_writer.h"

namespace lsmkv {

class MemTable;
class TableCache;
class TableBuilder;

// ============================================================================
// Concurrency design (the full write-up is in README.md, "Concurrency").
//
// Locks, in acquisition order (a thread holding one may only take locks
// listed below it):
//
//   1. mu_          DB state: writer queue, mem_/imm_ pointers, VersionSet,
//                   background-work flags, bg_error_. Never held during
//                   file I/O on the hot path.
//   2. sv_mu_       shared_mutex over the published SuperVersion pointer.
//                   Readers: shared, a few ns. Installer: exclusive.
//   3. leaf locks   memtable shard mutexes, block-cache shard mutexes,
//                   table-cache mutex, metrics registry. Never held while
//                   acquiring any other lock.
//
// Write path (one Put):
//   a. take mu_, enqueue, wait until at the queue head (or done by a leader)
//   b. leader: make room (may swap memtable+WAL, may stall), claim a group
//      of queued writers, assign their sequence numbers; release mu_
//   c. leader: append ONE WAL record for the whole group (+ one fsync if
//      any member asked for sync) -- no lock held
//   d. leader: take mu_, publish last_sequence, mark group done, wake the
//      next leader and the followers; release mu_
//   e. every writer in parallel: insert its own batch into the memtable
//      (shard mutex only), then ack
//
// Invariant that makes (e) safe: a memtable is never switched while an
// acknowledged-to-WAL writer has not finished inserting into it (the
// leader waits for pending_inserts_ == 0 before the switch). Otherwise a
// write could land in the new memtable while its WAL record is in the old,
// soon-deleted, log.
//
// Read path: take sv_mu_ shared, copy one shared_ptr<SuperVersion>
// {mem, imm, current Version}, release; search mem -> imm -> SSTables with
// no lock held.
// ============================================================================

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;
  ~DBImpl() override;

  Status Write(const WriteOptions& options, WriteBatch* updates) override;
  Status Get(const ReadOptions& options, const Slice& key,
             std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;
  bool GetProperty(const Slice& property, std::string* value) override;
  Status Flush() override;
  Status WaitForCompactions() override;
  Status CompactAll() override;

  // Exposed for tests.
  int NumLevelFiles(int level);
  SequenceNumber LastSequence() const { return versions_->LastSequence(); }

 private:
  friend class DB;
  struct Writer;
  struct CompactionState;

  // The unit readers pin: everything a read needs, captured atomically.
  struct SuperVersion {
    std::shared_ptr<MemTable> mem;
    std::shared_ptr<MemTable> imm;  // may be null
    std::shared_ptr<Version> current;
  };

  // Per-level compaction accounting for "lsmkv.stats".
  struct LevelStats {
    uint64_t micros = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    uint64_t count = 0;
  };

  // --- Open / recovery -------------------------------------------------------
  Status Recover(VersionEdit* edit, std::unique_lock<std::mutex>* lock);
  Status NewDB();
  Status RecoverLogFile(uint64_t log_number, bool last_log,
                        VersionEdit* edit, SequenceNumber* max_sequence,
                        bool* stream_damaged,
                        std::unique_lock<std::mutex>* lock);
  void SetupMetrics();
  void StartBackgroundThread();

  // --- Write path --------------------------------------------------------------
  Status MakeRoomForWrite(std::unique_lock<std::mutex>* lock, bool force);
  // Waits until a leader has committed w's batch or made w the leader.
  void AwaitCommitOrLeadership(Writer* w);
  void SetWriterState(Writer* w, int state);  // REQUIRES: mu_ held
  WriteBatch* BuildBatchGroup(Writer** last_writer);
  Status InsertIntoMemTable(Writer* w);
  void WaitForPendingInserts(std::unique_lock<std::mutex>* lock);
  void RecordStall(const char* reason, uint64_t start_micros);

  // --- SuperVersion ------------------------------------------------------------
  std::shared_ptr<const SuperVersion> GetSuperVersion() const;
  void InstallSuperVersion();  // REQUIRES: mu_ held

  // --- Background work -----------------------------------------------------------
  void BackgroundThreadMain();
  bool HasBackgroundWork() const;  // REQUIRES: mu_ held
  void BackgroundFlush(std::unique_lock<std::mutex>* lock);
  void BackgroundCompaction(std::unique_lock<std::mutex>* lock);
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                          std::unique_lock<std::mutex>* lock);
  Status DoCompactionWork(CompactionState* compact,
                          std::unique_lock<std::mutex>* lock);
  Status OpenCompactionOutputFile(CompactionState* compact,
                                  std::unique_lock<std::mutex>* lock);
  Status FinishCompactionOutputFile(CompactionState* compact);
  Status InstallCompactionResults(CompactionState* compact,
                                  std::unique_lock<std::mutex>* lock);
  void CleanupCompaction(CompactionState* compact);
  void RemoveObsoleteFiles(std::unique_lock<std::mutex>* lock);
  void RecordBackgroundError(const Status& s);  // REQUIRES: mu_ held
  void UpdateLevelGauges();                     // REQUIRES: mu_ held

  std::string StatsString();
  std::string JsonStats();

  // --- Constant after construction ----------------------------------------------
  Env* const env_;
  const InternalKeyComparator internal_comparator_;
  const Options options_;  // sanitized copy; options_.comparator is the user's
  const std::string dbname_;
  std::shared_ptr<Logger> logger_;
  std::shared_ptr<MetricsRegistry> metrics_;
  std::shared_ptr<Cache> block_cache_;
  TableOptions table_options_;
  std::shared_ptr<TableCache> table_cache_;
  std::unique_ptr<FileLock> db_lock_;

  // --- Guarded by mu_ ------------------------------------------------------------
  std::mutex mu_;
  std::condition_variable work_cv_;     // background thread waits for work
  std::condition_variable done_cv_;     // foreground waits for bg progress
  std::condition_variable inserts_cv_;  // memtable switch waits for inserts
  std::atomic<bool> shutting_down_{false};
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // being flushed, or null
  std::atomic<bool> has_imm_{false};
  std::unique_ptr<WritableFile> logfile_;
  uint64_t logfile_number_ = 0;
  std::unique_ptr<log::Writer> log_;
  std::deque<Writer*> writers_;
  WriteBatch tmp_batch_;
  std::set<uint64_t> pending_outputs_;  // table files being written
  bool bg_active_ = false;
  int manual_compaction_level_ = -1;  // >= 0 while a manual request pends
  bool manual_compaction_done_ = false;
  Status bg_error_;
  std::unique_ptr<VersionSet> versions_;
  LevelStats stats_[config::kNumLevels];
  std::thread bg_thread_;

  // Writers whose WAL record is committed but whose memtable insert is not
  // yet finished (see the invariant above).
  std::atomic<int64_t> pending_inserts_{0};
  std::atomic<bool> switch_waiting_{false};

  // --- Published read state ----------------------------------------------------
  mutable std::shared_mutex sv_mu_;
  std::shared_ptr<const SuperVersion> super_version_;

  // --- Accounting (lock-free) ----------------------------------------------------
  std::atomic<uint64_t> user_bytes_written_{0};
  std::atomic<uint64_t> wal_bytes_written_{0};
  std::atomic<uint64_t> flush_bytes_written_{0};
  std::atomic<uint64_t> compaction_bytes_written_{0};
  std::atomic<uint64_t> compaction_bytes_read_{0};
  std::atomic<uint64_t> stall_count_{0};
  std::atomic<uint64_t> stall_micros_{0};
  std::atomic<uint64_t> level_files_[config::kNumLevels] = {};
  std::atomic<uint64_t> level_bytes_[config::kNumLevels] = {};
  std::atomic<uint64_t> pending_compaction_bytes_{0};
  // Recovery accounting (written only during Open).
  uint64_t recovered_records_ = 0;
  uint64_t recovered_wal_bytes_ = 0;

  // Metric handles (owned by metrics_; valid for its lifetime).
  struct Metrics {
    Counter* puts = nullptr;
    Counter* deletes = nullptr;
    Counter* gets = nullptr;
    Counter* get_misses = nullptr;
    Counter* iterators = nullptr;
    Histogram* write_latency = nullptr;
    Histogram* get_latency = nullptr;
    Counter* write_groups = nullptr;
    Counter* write_group_writers = nullptr;
    Histogram* write_queue_wait = nullptr;  // enqueue -> committed or leader
    Histogram* wal_append = nullptr;        // leader's AddRecord (no fsync)
    Histogram* fsync_wal_seal = nullptr;    // fsync of a WAL being retired
    Histogram* fsync_sst = nullptr;         // fsync of a new SSTable
    Histogram* fsync_manifest = nullptr;    // fsync of a MANIFEST record
    Histogram* memtable_switch = nullptr;   // whole switch, writers blocked
    Counter* wal_bytes = nullptr;
    Counter* wal_syncs = nullptr;
    Histogram* wal_sync_latency = nullptr;
    Counter* user_bytes = nullptr;
    Counter* flushes = nullptr;
    Counter* flush_bytes = nullptr;
    Histogram* flush_latency = nullptr;
    Counter* compactions = nullptr;
    Counter* trivial_moves = nullptr;
    Histogram* compaction_latency = nullptr;
    Counter* compaction_read[config::kNumLevels] = {};
    Counter* compaction_written[config::kNumLevels] = {};
    Counter* stalls_memtable = nullptr;
    Counter* stalls_l0_stop = nullptr;
    Counter* stalls_l0_slowdown = nullptr;
    Counter* stall_micros = nullptr;
    Counter* bg_errors = nullptr;
    Gauge* recovery_wal_bytes = nullptr;
    Gauge* recovery_records = nullptr;
    Gauge* recovery_micros = nullptr;
  } m_;
  // Callback gauges read the atomics above. Declared last so they are
  // unregistered first, before anything they read is destroyed.
  std::vector<std::unique_ptr<MetricHandle>> metric_handles_;
};

// Fills in defaults for any unset policy objects.
Options SanitizeOptions(const std::string& dbname, const Options& src);

}  // namespace lsmkv

#endif  // LSMKV_SRC_DB_DB_IMPL_H_
