// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// micro-bench: component-level baselines, measured before any tuning.
//
//   skiplist vs std::map (insert / lookup, ns/op)
//   memtable insert & get, bytes per entry, overhead per entry
//   memtable insert scaling across threads: 1 shard vs 8 shards
//   CRC32C throughput, Bloom build/probe cost
//   WAL append throughput and fsync latency on this disk
//   LRU block-cache lookups under contention: 1 shard vs 16 shards

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "db/dbformat.h"
#include "lsmkv/cache.h"
#include "lsmkv/env.h"
#include "lsmkv/filter_policy.h"
#include "memtable/memtable.h"
#include "memtable/skiplist.h"
#include "sysinfo.h"
#include "util/arena.h"
#include "util/crc32c.h"
#include "util/histogram.h"
#include "util/random.h"
#include "wal/log_writer.h"

namespace lsmkv::bench {
namespace {

using Clock = std::chrono::steady_clock;

double Seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

struct U64Cmp {
  int operator()(const uint64_t& a, const uint64_t& b) const {
    return a < b ? -1 : (a > b ? 1 : 0);
  }
};

std::string Key16(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%013llu", static_cast<unsigned long long>(i));
  return buf;
}

std::ostringstream json;
bool first_field = true;

void Emit(const std::string& name, double value, const char* unit) {
  std::printf("  %-48s %14.2f %s\n", name.c_str(), value, unit);
  json << (first_field ? "" : ",") << "\"" << name << "\":" << value;
  first_field = false;
}

void BenchSkipList(uint64_t n) {
  std::printf("\n[skiplist vs std::map]  n=%llu random uint64 keys\n",
              static_cast<unsigned long long>(n));
  Random rnd(1);
  std::vector<uint64_t> keys(n);
  for (auto& k : keys) k = rnd.Next();

  Arena arena;
  SkipList<uint64_t, U64Cmp> list(U64Cmp(), &arena);
  auto t0 = Clock::now();
  for (uint64_t k : keys) list.Insert(k);
  auto t1 = Clock::now();
  Emit("skiplist_insert_ns_per_op", Seconds(t0, t1) * 1e9 / n, "ns/op");

  std::vector<uint64_t> probe = keys;
  std::shuffle(probe.begin(), probe.end(), std::mt19937_64(7));
  size_t found = 0;
  t0 = Clock::now();
  for (uint64_t k : probe) found += list.Contains(k);
  t1 = Clock::now();
  Emit("skiplist_lookup_ns_per_op", Seconds(t0, t1) * 1e9 / n, "ns/op");
  Emit("skiplist_bytes_per_node", static_cast<double>(arena.MemoryUsage()) / n, "B");

  std::map<uint64_t, uint64_t> m;
  t0 = Clock::now();
  for (uint64_t k : keys) m.emplace(k, 0);
  t1 = Clock::now();
  Emit("std_map_insert_ns_per_op", Seconds(t0, t1) * 1e9 / n, "ns/op");
  t0 = Clock::now();
  for (uint64_t k : probe) found += m.count(k);
  t1 = Clock::now();
  Emit("std_map_lookup_ns_per_op", Seconds(t0, t1) * 1e9 / n, "ns/op");
  if (found != 2 * n) std::printf("  !! lookup mismatch\n");
}

void BenchMemTable(uint64_t n) {
  std::printf("\n[memtable]  n=%llu, 16 B keys, 100 B values\n",
              static_cast<unsigned long long>(n));
  InternalKeyComparator icmp(BytewiseComparator());
  const std::string value(100, 'v');
  for (int shards : {1, 8}) {
    auto mem = std::make_shared<MemTable>(icmp, shards);
    Random rnd(3);
    std::vector<uint64_t> order(n);
    for (uint64_t i = 0; i < n; i++) order[i] = i;
    std::shuffle(order.begin(), order.end(), std::mt19937_64(11));
    auto t0 = Clock::now();
    SequenceNumber seq = 1;
    for (uint64_t i : order) mem->Add(seq++, kTypeValue, Key16(i), value);
    auto t1 = Clock::now();
    const std::string tag = "memtable_" + std::to_string(shards) + "shard_";
    Emit(tag + "insert_ns_per_op", Seconds(t0, t1) * 1e9 / n, "ns/op");
    std::string v;
    Status s;
    t0 = Clock::now();
    for (uint64_t i = 0; i < n; i++) {
      mem->Get(LookupKey(Key16(order[i]), kMaxSequenceNumber), &v, &s);
    }
    t1 = Clock::now();
    Emit(tag + "get_ns_per_op", Seconds(t0, t1) * 1e9 / n, "ns/op");
    if (shards == 1) {
      const double per_entry = static_cast<double>(mem->ApproximateMemoryUsage()) / n;
      const double payload = static_cast<double>(mem->RawDataBytes()) / n;
      Emit("memtable_bytes_per_entry", per_entry, "B");
      Emit("memtable_overhead_bytes_per_entry", per_entry - payload, "B");
    }
  }
}

void BenchMemTableScaling(uint64_t total) {
  std::printf("\n[memtable insert scaling]  %llu inserts split across threads\n",
              static_cast<unsigned long long>(total));
  InternalKeyComparator icmp(BytewiseComparator());
  const std::string value(100, 'v');
  for (int shards : {1, 8}) {
    for (int threads : {1, 2, 4, 8}) {
      auto mem = std::make_shared<MemTable>(icmp, shards);
      std::atomic<uint64_t> seq{1};
      const uint64_t per = total / static_cast<uint64_t>(threads);
      auto t0 = Clock::now();
      std::vector<std::thread> ts;
      for (int t = 0; t < threads; t++) {
        ts.emplace_back([&, t] {
          for (uint64_t i = 0; i < per; i++) {
            const uint64_t k = i * static_cast<uint64_t>(threads) + static_cast<uint64_t>(t);
            mem->Add(seq.fetch_add(1, std::memory_order_relaxed), kTypeValue,
                     Key16((k * 2654435761ull) % (total * 4)), value);
          }
        });
      }
      for (auto& th : ts) th.join();
      auto t1 = Clock::now();
      Emit("memtable_" + std::to_string(shards) + "shard_" + std::to_string(threads) +
               "thr_mops",
           static_cast<double>(per * threads) / Seconds(t0, t1) / 1e6, "M inserts/s");
    }
  }
}

void BenchCrcAndBloom() {
  std::printf("\n[crc32c / bloom]\n");
  std::string buf(4096, 'x');
  Random rnd(5);
  for (auto& c : buf) c = static_cast<char>(rnd.Next());
  const int iters = 200000;
  uint32_t acc = 0;
  auto t0 = Clock::now();
  for (int i = 0; i < iters; i++) acc ^= crc32c::Value(buf.data(), buf.size());
  auto t1 = Clock::now();
  Emit("crc32c_gb_per_sec", 4096.0 * iters / Seconds(t0, t1) / 1e9, "GB/s");
  if (acc == 42) std::printf(" ");

  auto policy = NewBloomFilterPolicy(10);
  const int n = 100000;
  std::vector<std::string> keys;
  for (int i = 0; i < n; i++) keys.push_back(Key16(static_cast<uint64_t>(i)));
  std::vector<Slice> slices(keys.begin(), keys.end());
  std::string filter;
  t0 = Clock::now();
  policy->CreateFilter(slices.data(), n, &filter);
  t1 = Clock::now();
  Emit("bloom_build_ns_per_key", Seconds(t0, t1) * 1e9 / n, "ns/key");
  int hits = 0;
  t0 = Clock::now();
  for (int i = 0; i < n; i++) hits += policy->KeyMayMatch(Key16(static_cast<uint64_t>(i) + n), filter);
  t1 = Clock::now();
  Emit("bloom_probe_ns", Seconds(t0, t1) * 1e9 / n, "ns/probe");
  Emit("bloom_fp_rate_10bits", static_cast<double>(hits) / n, "");
}

void BenchWal(const std::string& dir) {
  std::printf("\n[wal]  file in %s\n", dir.c_str());
  Env* env = Env::Default();
  (void)env->CreateDir(dir);
  const std::string fname = dir + "/micro_wal.log";
  std::unique_ptr<WritableFile> file;
  if (!env->NewWritableFile(fname, &file).ok()) return;
  log::Writer writer(file.get());
  const std::string record(128, 'r');
  const int n = 200000;
  auto t0 = Clock::now();
  for (int i = 0; i < n; i++) (void)writer.AddRecord(record);
  auto t1 = Clock::now();
  Emit("wal_append_128B_records_per_sec", n / Seconds(t0, t1), "rec/s");
  Emit("wal_append_mb_per_sec", 135.0 * n / Seconds(t0, t1) / 1e6, "MB/s");

  HdrHistogram fsync;
  for (int i = 0; i < 200; i++) {
    (void)writer.AddRecord(std::string(4096, 's'));
    const auto a = Clock::now();
    (void)file->Sync();
    fsync.Record(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - a).count()));
  }
  Emit("fsync_p50_us", static_cast<double>(fsync.Percentile(50)) / 1000.0, "us");
  Emit("fsync_p99_us", static_cast<double>(fsync.Percentile(99)) / 1000.0, "us");
  (void)file->Close();
  file.reset();
  (void)env->RemoveFile(fname);
}

void BenchCache() {
  std::printf("\n[LRU block cache]  8 threads, 90%% hits\n");
  for (int bits : {0, 4}) {
    auto cache = NewLRUCache(100000, bits);
    for (int i = 0; i < 100000; i++) {
      char k[8];
      std::memcpy(k, &i, sizeof(i));
      cache->Insert(Slice(k, sizeof(i)), std::make_shared<int>(i), 1);
    }
    const int threads = 8;
    const int per = 500000;
    auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; t++) {
      ts.emplace_back([&, t] {
        Random rnd(static_cast<uint64_t>(t) + 1);
        for (int i = 0; i < per; i++) {
          const int key = static_cast<int>(rnd.Uniform(111111));
          char k[8];
          std::memcpy(k, &key, sizeof(key));
          auto v = cache->Lookup(Slice(k, sizeof(key)));
        }
      });
    }
    for (auto& th : ts) th.join();
    auto t1 = Clock::now();
    Emit("cache_" + std::to_string(1 << bits) + "shard_8thr_mlookups",
         static_cast<double>(threads) * per / Seconds(t0, t1) / 1e6, "M lookups/s");
  }
}

}  // namespace
}  // namespace lsmkv::bench

int main(int argc, char** argv) {
  using namespace lsmkv::bench;
  std::string out;
  std::string dir = "micro-bench-tmp";
  uint64_t n = 1000000;
  for (int i = 1; i < argc; i++) {
    if (std::strncmp(argv[i], "--out=", 6) == 0) out = argv[i] + 6;
    if (std::strncmp(argv[i], "--dir=", 6) == 0) dir = argv[i] + 6;
    if (std::strncmp(argv[i], "--n=", 4) == 0) n = std::strtoull(argv[i] + 4, nullptr, 10);
  }
  KeepSystemAwake();
  const SystemInfo sys = CollectSystemInfo(argc, argv);
  std::printf("micro-bench  %s | %u hw threads | %s\n  %s\n", sys.cpu_model.c_str(),
              sys.hardware_threads, sys.os.c_str(), sys.build_flags.c_str());
  BenchSkipList(n);
  BenchMemTable(n);
  BenchMemTableScaling(n);
  BenchCrcAndBloom();
  BenchWal(dir);
  BenchCache();
  if (!out.empty()) {
    std::ofstream f(out);
    f << "{\"system\":" << ToJson(sys) << ",\"results\":{" << json.str() << "}}\n";
    std::printf("\nwrote %s\n", out.c_str());
  }
  return 0;
}
