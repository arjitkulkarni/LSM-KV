// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Options, ReadOptions, WriteOptions: plain value types that configure the
// engine. Every field has a defensible default; see README "Tuning".

#ifndef LSMKV_INCLUDE_LSMKV_OPTIONS_H_
#define LSMKV_INCLUDE_LSMKV_OPTIONS_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lsmkv {

class Cache;
class Comparator;
class Env;
class FilterPolicy;
class Logger;
class MetricsRegistry;

// How Open() treats a damaged write-ahead log.
enum class WalRecoveryMode : unsigned char {
  // A bad record is accepted only if nothing valid follows it anywhere in the
  // WAL stream: that is the signature of a torn (un-acked) final write. A bad
  // record followed by a good one is real corruption and Open() fails rather
  // than silently dropping acknowledged writes. Default.
  kTolerateCorruptedTail = 0,
  // Any damage, including a torn tail, fails Open().
  kAbsoluteConsistency = 1,
  // Stop replay at the first damage and open with the consistent prefix,
  // even if valid records follow. Trades durability for availability.
  kPointInTimeRecovery = 2,
};

struct Options {
  // ---- Policies (Strategy objects) ---------------------------------------
  // Key order. Non-owning; must outlive the DB. Default: bytewise.
  const Comparator* comparator = nullptr;
  // SSTable filter. nullptr => Bloom filter with bloom_bits_per_key.
  std::shared_ptr<const FilterPolicy> filter_policy;
  int bloom_bits_per_key = 10;  // <= 0 disables the default filter
  // Data-block cache, may be shared across DBs. nullptr => private LRU of
  // block_cache_capacity bytes.
  std::shared_ptr<Cache> block_cache;
  size_t block_cache_capacity = 8 << 20;
  // OS interface. Non-owning. nullptr => Env::Default().
  Env* env = nullptr;
  // Structured (JSON-lines) event log. nullptr => <dbname>/LOG.
  std::shared_ptr<Logger> info_log;
  // Metrics sink. nullptr => a private registry (still readable through
  // DB::GetProperty("lsmkv.prometheus")).
  std::shared_ptr<MetricsRegistry> metrics;

  // ---- Lifecycle ------------------------------------------------------------
  bool create_if_missing = true;
  bool error_if_exists = false;
  WalRecoveryMode wal_recovery_mode = WalRecoveryMode::kTolerateCorruptedTail;

  // ---- Write path -------------------------------------------------------------
  // Active memtable size that triggers a switch to a new memtable + WAL.
  size_t write_buffer_size = 4 << 20;
  // Hash shards of the memtable, each with its own writer mutex.
  // 1 reproduces the single-lock baseline used in BENCHMARKS.md.
  int memtable_shards = 8;
  // Upper bound on the bytes a group-commit leader folds into one WAL record.
  size_t max_write_group_bytes = 1 << 20;
  // How a queued writer waits for its commit group. true: spin briefly,
  // then yield, then block (a group commit takes a few microseconds, an OS
  // sleep/wake round trip several times that). false: block on a condition
  // variable immediately -- the pre-optimization baseline kept for A/B runs
  // (see BENCHMARKS.md, "Performance pass").
  bool adaptive_write_wait = true;
  // Record per-operation latency histograms (two clock reads per op).
  bool enable_latency_metrics = true;

  // ---- SSTable format -------------------------------------------------------
  size_t block_size = 4096;
  int block_restart_interval = 16;
  size_t max_file_size = 2 << 20;
  int max_open_files = 1000;

  // ---- Leveled compaction -----------------------------------------------------
  int l0_compaction_trigger = 4;
  int l0_slowdown_writes_trigger = 8;
  int l0_stop_writes_trigger = 12;
  uint64_t max_bytes_for_level_base = 10ull << 20;
  int max_bytes_for_level_multiplier = 10;
  // Test hook: never schedule compactions (flushes still run).
  bool disable_auto_compactions = false;
};

struct ReadOptions {
  // Checksums of blocks read from disk are always verified; this adds
  // verification of blocks served from the cache as well.
  bool verify_checksums = false;
  // Populate the block cache with blocks read by this operation. Scans that
  // touch a lot of data once should set this to false.
  bool fill_cache = true;
};

struct WriteOptions {
  // false: the write is acked once the WAL record reaches the OS page cache
  //        (survives a process crash, not a power loss).
  // true:  the WAL is fsync'd before the ack (survives power loss). Group
  //        commit amortizes one fsync across every concurrent writer.
  bool sync = false;
};

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_OPTIONS_H_
