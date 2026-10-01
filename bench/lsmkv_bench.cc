// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// lsmkv-bench: a YCSB-style load generator with a methodology you can defend.
//
//   * Workloads A, B, C, D, F; uniform, zipfian (theta 0.99, scrambled) or
//     latest key choice.
//   * Fresh load per repetition, then WaitForCompactions() so every rep
//     starts from the same settled LSM shape.
//   * Warmup (not measured), then a fixed-duration steady state.
//   * Per-thread HDR-style histograms (<= 0.8% error), merged at the end.
//   * Open-loop mode (--rate): operations are *scheduled* at a fixed rate and
//     latency is measured from the scheduled start, not the actual start.
//     A closed-loop generator waits for each op to finish before sending the
//     next, so when the server stalls it silently stops sending -- and never
//     records the latency the missing requests would have seen ("coordinated
//     omission"). Both views are reported.
//   * N repetitions; the median of each metric is reported.
//   * Hardware, OS, compiler, flags and the literal command line are written
//     into the JSON result.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "leveldb_adapter.h"
#include "lsmkv/db.h"
#include "lsmkv/metrics.h"
#include "server/metrics_http_server.h"
#include "sysinfo.h"
#include "util/histogram.h"
#include "util/random.h"
#include "ycsb.h"

namespace lsmkv::bench {
namespace {

using Clock = std::chrono::steady_clock;

inline uint64_t NowNanos() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

struct Config {
  std::string engine = "lsmkv";
  char workload = 'A';
  std::string distribution;  // empty => workload default
  int threads = 8;
  uint64_t records = 1000000;
  size_t value_size = 100;
  double warmup_s = 10;
  double duration_s = 60;
  int reps = 3;
  double rate = 0;  // total ops/s; 0 => closed loop
  std::string db = "bench-db";
  bool sync = false;
  int memtable_shards = 8;
  size_t write_buffer = 4 << 20;
  int bloom_bits = 10;
  size_t cache_mb = 8;
  std::string out;
  std::string timeseries;
  int metrics_port = -1;
  std::string metrics_bind = "127.0.0.1";
  uint64_t seed = 42;
  bool latency_metrics = true;
  bool load_only = false;
  bool adaptive_wait = true;
  std::string label;
};

// db_bench-style value source: slices of one pre-generated random buffer,
// so producing a value costs nothing measurable.
class ValueSource {
 public:
  explicit ValueSource(uint64_t seed) {
    Random rnd(seed);
    data_.resize(1 << 20);
    for (auto& c : data_) c = static_cast<char>(' ' + rnd.Uniform(95));
  }
  Slice Get(Random* rnd, size_t len) const {
    const size_t pos = rnd->Uniform(data_.size() - len);
    return Slice(data_.data() + pos, len);
  }

 private:
  std::string data_;
};

struct ThreadResult {
  HdrHistogram latency[static_cast<int>(OpType::kCount)];  // from intended start
  HdrHistogram service[static_cast<int>(OpType::kCount)];  // from actual start
  uint64_t ops[static_cast<int>(OpType::kCount)] = {};
  uint64_t errors = 0;
};

struct LatencySummary {
  uint64_t count = 0;
  double mean_us = 0, p50_us = 0, p99_us = 0, p999_us = 0, max_us = 0;
};

LatencySummary Summarize(const HdrHistogram& h) {
  LatencySummary s;
  s.count = h.Count();
  s.mean_us = h.Mean() / 1000.0;
  s.p50_us = static_cast<double>(h.Percentile(50)) / 1000.0;
  s.p99_us = static_cast<double>(h.Percentile(99)) / 1000.0;
  s.p999_us = static_cast<double>(h.Percentile(99.9)) / 1000.0;
  s.max_us = static_cast<double>(h.Max()) / 1000.0;
  return s;
}

std::string ToJson(const LatencySummary& s) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "{\"count\":%llu,\"mean\":%.3f,\"p50\":%.3f,\"p99\":%.3f,\"p999\":%.3f,"
                "\"max\":%.3f}",
                static_cast<unsigned long long>(s.count), s.mean_us, s.p50_us, s.p99_us,
                s.p999_us, s.max_us);
  return buf;
}

struct RepResult {
  double load_seconds = 0;
  double load_ops_per_sec = 0;
  double throughput = 0;
  uint64_t measured_ops = 0;
  uint64_t errors = 0;
  LatencySummary latency[static_cast<int>(OpType::kCount) + 1];  // + "all"
  LatencySummary service[static_cast<int>(OpType::kCount) + 1];
  std::vector<std::pair<uint64_t, double>> cdf;  // all ops, latency view
  std::string engine_stats = "{}";
  std::string timeseries_json = "[]";
  // Write-path diagnostics from the engine's own metrics (whole rep).
  double mean_group_size = 0;
  double queue_wait_p50_us = 0, queue_wait_p99_us = 0;
  double wal_append_p50_us = 0, wal_append_p99_us = 0;
  // Worst-case background I/O over the rep (bucket upper bound of the max).
  std::string background_json = "{}";
  std::string background_line;
};

Status OpenEngine(const Config& c, const std::string& dir,
                  std::shared_ptr<MetricsRegistry> registry, std::unique_ptr<DB>* db) {
  if (c.engine == "leveldb") {
#if defined(LSMKV_WITH_LEVELDB)
    LevelDBAdapter::Config lc;
    lc.write_buffer_size = c.write_buffer;
    lc.block_cache_bytes = c.cache_mb << 20;
    lc.bloom_bits = c.bloom_bits;
    return LevelDBAdapter::Open(lc, dir, db);
#else
    return Status::NotSupported("rebuild with -DLSMKV_WITH_LEVELDB=ON");
#endif
  }
  Options o;
  o.write_buffer_size = c.write_buffer;
  o.memtable_shards = c.memtable_shards;
  o.bloom_bits_per_key = c.bloom_bits;
  o.block_cache_capacity = c.cache_mb << 20;
  o.metrics = std::move(registry);
  o.enable_latency_metrics = c.latency_metrics;
  o.adaptive_write_wait = c.adaptive_wait;
  return DB::Open(o, dir, db);
}

// Sleeps/spins until the steady clock reaches `target_ns`.
void WaitUntil(uint64_t target_ns) {
  while (true) {
    const uint64_t now = NowNanos();
    if (now >= target_ns) return;
    const uint64_t remaining = target_ns - now;
    if (remaining > 2000000) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(remaining - 1000000));
    } else if (remaining > 20000) {
      std::this_thread::yield();
    }
  }
}

std::string MetricValue(MetricsRegistry* r, const std::string& name,
                        const MetricLabels& labels = {}) {
  double v = 0;
  if (r == nullptr || !r->GetValue(name, labels, &v)) return "null";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  return buf;
}

RepResult RunOnce(const Config& c, const Workload& w, int rep, bool capture_timeseries) {
  RepResult result;
  const std::string dir = c.db + "/rep" + std::to_string(rep);
  (void)DestroyDB(dir, Options());
  auto registry = std::make_shared<MetricsRegistry>();
  std::unique_ptr<DB> db;
  Status s = OpenEngine(c, dir, registry, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    std::exit(1);
  }
  std::unique_ptr<MetricsHttpServer> server;
  if (c.metrics_port >= 0) {
    MetricsRegistry* raw = registry.get();
    s = MetricsHttpServer::Start(c.metrics_bind, c.metrics_port,
                                 [raw] { return raw->ExportPrometheus(); }, &server);
    if (s.ok()) {
      std::fprintf(stderr, "metrics: http://%s:%d/metrics\n", c.metrics_bind.c_str(),
                   server->port());
    } else {
      std::fprintf(stderr, "metrics server: %s\n", s.ToString().c_str());
    }
  }
  ValueSource values(c.seed);
  WriteOptions wopt;
  wopt.sync = c.sync;

  // ---- Load ----------------------------------------------------------------------
  {
    std::atomic<uint64_t> next{0};
    const uint64_t t0 = NowNanos();
    std::vector<std::thread> threads;
    std::vector<HdrHistogram> load_lat(static_cast<size_t>(c.threads));
    for (int t = 0; t < c.threads; t++) {
      threads.emplace_back([&, t] {
        Random rnd(c.seed * 131 + static_cast<uint64_t>(t));
        while (true) {
          const uint64_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= c.records) break;
          const uint64_t a = NowNanos();
          Status ps = db->Put(wopt, YcsbKey(i), values.Get(&rnd, c.value_size));
          load_lat[static_cast<size_t>(t)].Record(NowNanos() - a);
          if (!ps.ok()) {
            std::fprintf(stderr, "load put failed: %s\n", ps.ToString().c_str());
            std::exit(1);
          }
        }
      });
    }
    for (auto& th : threads) th.join();
    result.load_seconds = static_cast<double>(NowNanos() - t0) / 1e9;
    result.load_ops_per_sec = static_cast<double>(c.records) / result.load_seconds;
    if (c.load_only) {
      HdrHistogram all;
      for (auto& h : load_lat) all.Merge(h);
      result.latency[static_cast<int>(OpType::kCount)] = Summarize(all);
      result.latency[static_cast<int>(OpType::kInsert)] = Summarize(all);
      result.throughput = result.load_ops_per_sec;
      result.measured_ops = c.records;
      result.cdf = all.Cdf();
    }
  }
  (void)db->WaitForCompactions();

  if (!c.load_only) {
    // ---- Run -------------------------------------------------------------------
    std::atomic<uint64_t> inserted{c.records};
    std::atomic<uint64_t> progress{0};
    std::vector<ThreadResult> results(static_cast<size_t>(c.threads));
    const uint64_t start_ns = NowNanos() + 50000000;  // 50 ms to line up
    const uint64_t measure_ns = start_ns + static_cast<uint64_t>(c.warmup_s * 1e9);
    const uint64_t end_ns = measure_ns + static_cast<uint64_t>(c.duration_s * 1e9);
    const double per_thread_rate = c.rate > 0 ? c.rate / c.threads : 0;
    const uint64_t interval_ns =
        per_thread_rate > 0 ? static_cast<uint64_t>(1e9 / per_thread_rate) : 0;
    const std::string dist = c.distribution.empty() ? w.default_distribution : c.distribution;

    // Once-per-second samples of throughput and engine internals.
    std::atomic<bool> sampling{true};
    std::string series = "[";
    std::thread sampler;
    if (capture_timeseries) {
      sampler = std::thread([&] {
        uint64_t last_ops = 0;
        uint64_t tick = start_ns;
        bool first = true;
        while (sampling.load()) {
          tick += 1000000000;
          WaitUntil(tick);
          const uint64_t ops = progress.load(std::memory_order_relaxed);
          MetricsRegistry* r = registry.get();
          char head[128];
          std::snprintf(head, sizeof(head), "{\"t\":%.0f,\"ops_per_sec\":%llu",
                        static_cast<double>(tick - start_ns) / 1e9,
                        static_cast<unsigned long long>(ops - last_ops));
          last_ops = ops;
          std::string row = head;
          row += ",\"memtable_bytes\":" + MetricValue(r, "lsmkv_memtable_bytes");
          row += ",\"l0_files\":" + MetricValue(r, "lsmkv_level_files", {{"level", "0"}});
          row += ",\"pending_compaction_bytes\":" +
                 MetricValue(r, "lsmkv_compaction_pending_bytes");
          row += ",\"stalls_memtable\":" +
                 MetricValue(r, "lsmkv_write_stalls_total", {{"reason", "memtable_full"}});
          row += ",\"stalls_l0_stop\":" +
                 MetricValue(r, "lsmkv_write_stalls_total", {{"reason", "l0_stop"}});
          row += ",\"stalls_l0_slowdown\":" +
                 MetricValue(r, "lsmkv_write_stalls_total", {{"reason", "l0_slowdown"}});
          row += ",\"stall_micros\":" + MetricValue(r, "lsmkv_write_stall_micros_total");
          row += ",\"flushes\":" + MetricValue(r, "lsmkv_flushes_total");
          row += ",\"compactions\":" + MetricValue(r, "lsmkv_compactions_total");
          row += ",\"cache_hit_ratio\":" + MetricValue(r, "lsmkv_block_cache_hit_ratio");
          row += "}";
          series += (first ? "" : ",") + row;
          first = false;
        }
      });
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < c.threads; t++) {
      threads.emplace_back([&, t] {
        ThreadResult& tr = results[static_cast<size_t>(t)];
        Random rnd(c.seed * 7777 + static_cast<uint64_t>(t) * 31 + static_cast<uint64_t>(rep));
        std::unique_ptr<KeyChooser> chooser = MakeChooser(dist, c.records, &inserted);
        std::string value;
        // Stagger threads within one interval so open-loop arrivals do not
        // come in bursts of `threads`.
        uint64_t next = start_ns + (interval_ns * static_cast<uint64_t>(t)) /
                                       static_cast<uint64_t>(c.threads);
        WaitUntil(start_ns);
        while (true) {
          uint64_t intended;
          if (interval_ns > 0) {
            WaitUntil(next);
            intended = next;
            next += interval_ns;
            if (intended >= end_ns) break;
            // The window is over even if we are behind schedule: stop rather
            // than drain the backlog. Draining would make "achieved" equal
            // "offered" no matter how overloaded the engine was.
            if (NowNanos() >= end_ns) break;
          } else {
            intended = NowNanos();
            if (intended >= end_ns) break;
          }

          const double dice = rnd.NextDouble();
          OpType op;
          if (dice < w.read) {
            op = OpType::kRead;
          } else if (dice < w.read + w.update) {
            op = OpType::kUpdate;
          } else if (dice < w.read + w.update + w.insert) {
            op = OpType::kInsert;
          } else {
            op = OpType::kReadModifyWrite;
          }

          const uint64_t actual = NowNanos();
          Status os;
          switch (op) {
            case OpType::kRead:
              os = db->Get(ReadOptions(), YcsbKey(chooser->Next(&rnd)), &value);
              if (os.IsNotFound()) os = Status::OK();
              break;
            case OpType::kUpdate:
              os = db->Put(wopt, YcsbKey(chooser->Next(&rnd)), values.Get(&rnd, c.value_size));
              break;
            case OpType::kInsert: {
              const uint64_t k = inserted.fetch_add(1, std::memory_order_relaxed);
              os = db->Put(wopt, YcsbKey(k), values.Get(&rnd, c.value_size));
              break;
            }
            case OpType::kReadModifyWrite: {
              const std::string key = YcsbKey(chooser->Next(&rnd));
              os = db->Get(ReadOptions(), key, &value);
              if (os.ok() || os.IsNotFound()) {
                // Modify: overwrite a prefix of the old value.
                std::string nv = values.Get(&rnd, c.value_size).ToString();
                if (value.size() > 8) nv.replace(0, 8, value, 0, 8);
                os = db->Put(wopt, key, nv);
              }
              break;
            }
            default:
              break;
          }
          const uint64_t done = NowNanos();
          progress.fetch_add(1, std::memory_order_relaxed);
          if (!os.ok()) tr.errors++;
          // Count only work that finished inside the measurement window.
          if (intended >= measure_ns && done <= end_ns) {
            const int i = static_cast<int>(op);
            tr.latency[i].Record(done - intended);
            tr.service[i].Record(done - actual);
            tr.ops[i]++;
          }
        }
      });
    }
    for (auto& th : threads) th.join();
    sampling.store(false);
    if (sampler.joinable()) sampler.join();
    series += "]";
    result.timeseries_json = series;

    constexpr int kAll = static_cast<int>(OpType::kCount);
    HdrHistogram all_lat;
    HdrHistogram all_svc;
    for (int op = 0; op < kAll; op++) {
      HdrHistogram lat;
      HdrHistogram svc;
      for (auto& tr : results) {
        lat.Merge(tr.latency[op]);
        svc.Merge(tr.service[op]);
        result.measured_ops += tr.ops[op];
      }
      result.latency[op] = Summarize(lat);
      result.service[op] = Summarize(svc);
      all_lat.Merge(lat);
      all_svc.Merge(svc);
    }
    for (auto& tr : results) result.errors += tr.errors;
    result.latency[kAll] = Summarize(all_lat);
    result.service[kAll] = Summarize(all_svc);
    result.throughput = static_cast<double>(result.measured_ops) / c.duration_s;
    result.cdf = all_lat.Cdf();
  }

  std::string stats;
  if (db->GetProperty("lsmkv.json-stats", &stats)) result.engine_stats = stats;
  {
    double groups = 0;
    double writers = 0;
    if (registry->GetValue("lsmkv_wal_group_commits_total", {}, &groups) &&
        registry->GetValue("lsmkv_wal_group_commit_writers_total", {}, &writers) &&
        groups > 0) {
      result.mean_group_size = writers / groups;
      Histogram* qw = registry->GetHistogram("lsmkv_write_queue_wait_seconds", "");
      Histogram* wa = registry->GetHistogram("lsmkv_wal_append_seconds", "");
      result.queue_wait_p50_us = static_cast<double>(qw->PercentileNanos(50)) / 1000.0;
      result.queue_wait_p99_us = static_cast<double>(qw->PercentileNanos(99)) / 1000.0;
      result.wal_append_p50_us = static_cast<double>(wa->PercentileNanos(50)) / 1000.0;
      result.wal_append_p99_us = static_cast<double>(wa->PercentileNanos(99)) / 1000.0;
    }
    // Background I/O: the worst fsync of each file type and the longest
    // memtable switch. A multi-second stall that closed-loop percentiles
    // hide shows up here.
    struct Probe { const char* key; const char* name; MetricLabels labels; };
    const Probe probes[] = {
        {"memtable_switch", "lsmkv_memtable_switch_seconds", {}},
        {"fsync_wal_seal", "lsmkv_fsync_seconds", {{"file", "wal_seal"}}},
        {"fsync_sst", "lsmkv_fsync_seconds", {{"file", "sst"}}},
        {"fsync_manifest", "lsmkv_fsync_seconds", {{"file", "manifest"}}},
        {"flush", "lsmkv_flush_seconds", {}},
        {"compaction", "lsmkv_compaction_seconds", {}},
    };
    std::string bj = "{";
    std::string line;
    bool first = true;
    for (const Probe& p : probes) {
      Histogram* h = registry->GetHistogram(p.name, "", p.labels);
      if (h == nullptr || h->Count() == 0) continue;
      const double max_ms = static_cast<double>(h->PercentileNanos(100)) / 1e6;
      const double p99_ms = static_cast<double>(h->PercentileNanos(99)) / 1e6;
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s\"%s\":{\"count\":%llu,\"p99_ms\":%.3f,\"max_ms\":%.3f}",
                    first ? "" : ",", p.key, static_cast<unsigned long long>(h->Count()),
                    p99_ms, max_ms);
      bj += buf;
      std::snprintf(buf, sizeof(buf), "%s%s max %.1f ms", first ? "" : " | ", p.key, max_ms);
      line += buf;
      first = false;
    }
    result.background_json = bj + "}";
    result.background_line = line;
  }
  server.reset();
  db.reset();
  (void)DestroyDB(dir, Options());
  return result;
}

double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  if (v.empty()) return 0;
  const size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

bool Flag(const char* arg, const char* name, std::string* value) {
  const size_t n = std::strlen(name);
  if (std::strncmp(arg, name, n) == 0 && arg[n] == '=') {
    *value = arg + n + 1;
    return true;
  }
  return false;
}

int Main(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; i++) {
    std::string v;
    if (Flag(argv[i], "--engine", &v)) c.engine = v;
    else if (Flag(argv[i], "--workload", &v)) {
      if (v == "load") {
        c.load_only = true;
      } else {
        c.workload = static_cast<char>(std::toupper(static_cast<unsigned char>(v[0])));
      }
    } else if (Flag(argv[i], "--distribution", &v)) c.distribution = v;
    else if (Flag(argv[i], "--threads", &v)) c.threads = std::max(1, std::atoi(v.c_str()));
    else if (Flag(argv[i], "--records", &v)) c.records = std::strtoull(v.c_str(), nullptr, 10);
    else if (Flag(argv[i], "--value_size", &v)) c.value_size = std::strtoull(v.c_str(), nullptr, 10);
    else if (Flag(argv[i], "--warmup", &v)) c.warmup_s = std::atof(v.c_str());
    else if (Flag(argv[i], "--duration", &v)) c.duration_s = std::atof(v.c_str());
    else if (Flag(argv[i], "--reps", &v)) c.reps = std::max(1, std::atoi(v.c_str()));
    else if (Flag(argv[i], "--rate", &v)) c.rate = std::atof(v.c_str());
    else if (Flag(argv[i], "--db", &v)) c.db = v;
    else if (Flag(argv[i], "--sync", &v)) c.sync = v == "1";
    else if (Flag(argv[i], "--shards", &v)) c.memtable_shards = std::atoi(v.c_str());
    else if (Flag(argv[i], "--write_buffer", &v)) c.write_buffer = std::strtoull(v.c_str(), nullptr, 10);
    else if (Flag(argv[i], "--bloom_bits", &v)) c.bloom_bits = std::atoi(v.c_str());
    else if (Flag(argv[i], "--cache_mb", &v)) c.cache_mb = std::strtoull(v.c_str(), nullptr, 10);
    else if (Flag(argv[i], "--out", &v)) c.out = v;
    else if (Flag(argv[i], "--timeseries", &v)) c.timeseries = v;
    else if (Flag(argv[i], "--metrics_port", &v)) c.metrics_port = std::atoi(v.c_str());
    else if (Flag(argv[i], "--metrics_bind", &v)) c.metrics_bind = v;
    else if (Flag(argv[i], "--seed", &v)) c.seed = std::strtoull(v.c_str(), nullptr, 10);
    else if (Flag(argv[i], "--latency_metrics", &v)) c.latency_metrics = v != "0";
    else if (Flag(argv[i], "--label", &v)) c.label = v;
    else if (Flag(argv[i], "--adaptive_wait", &v)) c.adaptive_wait = v != "0";
    else {
      std::fprintf(stderr, "unknown flag: %s\n", argv[i]);
      return 64;
    }
  }
  Workload w;
  if (!GetWorkload(c.workload, &w)) {
    std::fprintf(stderr, "unknown workload %c (use A B C D F or load)\n", c.workload);
    return 64;
  }
  const std::string dist = c.load_only ? "sequential-hashed"
                           : c.distribution.empty() ? w.default_distribution
                                                    : c.distribution;
  KeepSystemAwake();
  const SystemInfo sys = CollectSystemInfo(argc, argv);

  std::printf("lsmkv-bench  engine=%s  workload=%s  dist=%s  threads=%d  records=%llu x %zu B"
              "  %s\n",
              c.engine.c_str(), c.load_only ? "load" : std::string(1, c.workload).c_str(),
              dist.c_str(), c.threads, static_cast<unsigned long long>(c.records),
              c.value_size,
              c.rate > 0 ? ("open-loop @ " + std::to_string(static_cast<long long>(c.rate)) +
                            " ops/s").c_str()
                         : "closed-loop");
  std::printf("  %s | %u hw threads | %s\n", sys.cpu_model.c_str(), sys.hardware_threads,
              sys.os.c_str());
  if (!c.load_only) {
    std::printf("  %s; warmup %.0fs, measure %.0fs, %d reps (median reported)\n",
                w.description.c_str(), c.warmup_s, c.duration_s, c.reps);
  }

  std::vector<RepResult> reps;
  for (int r = 0; r < c.reps; r++) {
    reps.push_back(RunOnce(c, w, r, r == 0 && !c.timeseries.empty()));
    const RepResult& rr = reps.back();
    const LatencySummary& all = rr.latency[static_cast<int>(OpType::kCount)];
    std::printf("  rep %d: %10.0f ops/s   p50 %8.2f us   p99 %8.2f us   p99.9 %9.2f us"
                "   (load %.0f ops/s, errors %llu)\n",
                r + 1, rr.throughput, all.p50_us, all.p99_us, all.p999_us,
                rr.load_ops_per_sec, static_cast<unsigned long long>(rr.errors));
    if (c.rate > 0 && rr.throughput < 0.95 * c.rate) {
      std::printf("         OVERLOADED: offered %.0f ops/s, completed %.0f ops/s -- latency includes "
                  "an ever-growing queue\n",
                  c.rate, rr.throughput);
    }
    if (rr.mean_group_size > 0) {
      std::printf("         write path: mean group %.2f writers | queue wait p50 %.1f us p99 %.1f us"
                  " | WAL append p50 %.1f us p99 %.1f us\n",
                  rr.mean_group_size, rr.queue_wait_p50_us, rr.queue_wait_p99_us,
                  rr.wal_append_p50_us, rr.wal_append_p99_us);
    }
    if (!rr.background_line.empty()) {
      std::printf("         background: %s\n", rr.background_line.c_str());
    }
    std::fflush(stdout);
  }

  // Medians across reps, per metric.
  constexpr int kAll = static_cast<int>(OpType::kCount);
  std::vector<double> tput;
  std::vector<double> load;
  for (const auto& rr : reps) {
    tput.push_back(rr.throughput);
    load.push_back(rr.load_ops_per_sec);
  }
  auto median_of = [&](int op, bool service, double LatencySummary::*field) {
    std::vector<double> v;
    for (const auto& rr : reps) {
      const LatencySummary& s = service ? rr.service[op] : rr.latency[op];
      if (s.count > 0) v.push_back(s.*field);
    }
    return Median(v);
  };
  const double med_tput = Median(tput);
  std::printf("  median: %10.0f ops/s   p50 %8.2f us   p99 %8.2f us   p99.9 %9.2f us\n",
              med_tput, median_of(kAll, false, &LatencySummary::p50_us),
              median_of(kAll, false, &LatencySummary::p99_us),
              median_of(kAll, false, &LatencySummary::p999_us));
  for (int op = 0; op < kAll; op++) {
    if (reps[0].latency[op].count == 0) continue;
    std::printf("    %-7s p50 %8.2f us   p99 %8.2f us   p99.9 %9.2f us   (service p99 %8.2f us)\n",
                OpName(static_cast<OpType>(op)),
                median_of(op, false, &LatencySummary::p50_us),
                median_of(op, false, &LatencySummary::p99_us),
                median_of(op, false, &LatencySummary::p999_us),
                median_of(op, true, &LatencySummary::p99_us));
  }

  if (!c.out.empty()) {
    // Pick the rep whose throughput is the median for the CDF and stats.
    size_t median_rep = 0;
    double best = 1e300;
    for (size_t i = 0; i < reps.size(); i++) {
      const double d = std::abs(reps[i].throughput - med_tput);
      if (d < best) {
        best = d;
        median_rep = i;
      }
    }
    std::ostringstream o;
    o << "{\"label\":\"" << JsonEscape(c.label) << "\",\"system\":" << ToJson(sys)
      << ",\"config\":{\"engine\":\"" << c.engine << "\",\"workload\":\""
      << (c.load_only ? std::string("load") : std::string(1, c.workload))
      << "\",\"distribution\":\"" << dist << "\",\"threads\":" << c.threads
      << ",\"records\":" << c.records << ",\"value_size\":" << c.value_size
      << ",\"warmup_s\":" << c.warmup_s << ",\"duration_s\":" << c.duration_s
      << ",\"reps\":" << c.reps << ",\"rate\":" << c.rate << ",\"sync\":" << (c.sync ? "true" : "false")
      << ",\"memtable_shards\":" << c.memtable_shards << ",\"write_buffer\":" << c.write_buffer
      << ",\"bloom_bits\":" << c.bloom_bits << ",\"cache_mb\":" << c.cache_mb
      << ",\"adaptive_write_wait\":" << (c.adaptive_wait ? "true" : "false") << "},";
    o << "\"reps\":[";
    for (size_t i = 0; i < reps.size(); i++) {
      const RepResult& rr = reps[i];
      if (i) o << ",";
      char head[640];
      std::snprintf(head, sizeof(head),
                    "{\"throughput\":%.1f,\"measured_ops\":%llu,\"errors\":%llu,"
                    "\"offered_rate\":%.1f,\"overloaded\":%s,"
                    "\"load_seconds\":%.3f,\"load_ops_per_sec\":%.1f,"
                    "\"write_path\":{\"mean_group_size\":%.3f,\"queue_wait_p50_us\":%.2f,"
                    "\"queue_wait_p99_us\":%.2f,\"wal_append_p50_us\":%.2f,"
                    "\"wal_append_p99_us\":%.2f},",
                    rr.throughput, static_cast<unsigned long long>(rr.measured_ops),
                    static_cast<unsigned long long>(rr.errors), c.rate,
                    (c.rate > 0 && rr.throughput < 0.95 * c.rate) ? "true" : "false",
                    rr.load_seconds,
                    rr.load_ops_per_sec, rr.mean_group_size, rr.queue_wait_p50_us,
                    rr.queue_wait_p99_us, rr.wal_append_p50_us, rr.wal_append_p99_us);
      o << head << "\"latency_us\":{";
      bool first = true;
      for (int op = 0; op <= kAll; op++) {
        if (rr.latency[op].count == 0) continue;
        o << (first ? "" : ",") << "\""
          << (op == kAll ? "all" : OpName(static_cast<OpType>(op))) << "\":"
          << ToJson(rr.latency[op]);
        first = false;
      }
      o << "},\"service_us\":{";
      first = true;
      for (int op = 0; op <= kAll; op++) {
        if (rr.service[op].count == 0) continue;
        o << (first ? "" : ",") << "\""
          << (op == kAll ? "all" : OpName(static_cast<OpType>(op))) << "\":"
          << ToJson(rr.service[op]);
        first = false;
      }
      o << "},\"background\":" << rr.background_json << ",\"engine\":" << rr.engine_stats << "}";
    }
    o << "],\"median\":{\"throughput\":" << med_tput << ",\"load_ops_per_sec\":" << Median(load)
      << ",\"latency_us\":{";
    bool first = true;
    for (int op = 0; op <= kAll; op++) {
      if (reps[0].latency[op].count == 0) continue;
      char buf[256];
      std::snprintf(buf, sizeof(buf), "{\"p50\":%.3f,\"p99\":%.3f,\"p999\":%.3f,\"mean\":%.3f}",
                    median_of(op, false, &LatencySummary::p50_us),
                    median_of(op, false, &LatencySummary::p99_us),
                    median_of(op, false, &LatencySummary::p999_us),
                    median_of(op, false, &LatencySummary::mean_us));
      o << (first ? "" : ",") << "\"" << (op == kAll ? "all" : OpName(static_cast<OpType>(op)))
        << "\":" << buf;
      first = false;
    }
    o << "}},\"cdf_us\":[";
    const auto& cdf = reps[median_rep].cdf;
    // Thin the CDF to <= 400 points for plotting.
    const size_t step = std::max<size_t>(1, cdf.size() / 400);
    for (size_t i = 0; i < cdf.size(); i += step) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%s[%.3f,%.6f]", i ? "," : "",
                    static_cast<double>(cdf[i].first) / 1000.0, cdf[i].second);
      o << buf;
    }
    o << "]}";
    std::ofstream f(c.out);
    f << o.str() << "\n";
    std::printf("  wrote %s\n", c.out.c_str());
  }
  if (!c.timeseries.empty()) {
    std::ofstream f(c.timeseries);
    f << reps[0].timeseries_json << "\n";
    std::printf("  wrote %s\n", c.timeseries.c_str());
  }
  return 0;
}

}  // namespace
}  // namespace lsmkv::bench

int main(int argc, char** argv) { return lsmkv::bench::Main(argc, argv); }
