// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// bloom-sweep: what does each bit of Bloom filter per key buy?
//
// For bits/key in {0 (no filter), 4, 6, 8, 10, 14}:
//   1. Filter level: false-positive rate on keys never inserted, vs theory
//      (1 - e^(-k/b))^k.
//   2. Engine level: a DB with a realistic multi-level shape and the block
//      cache disabled, so every data-block fetch is a real file read. We
//      count SSTable data-block reads per *negative* lookup (key absent) and
//      per positive lookup, straight from the engine's counters.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lsmkv/db.h"
#include "lsmkv/filter_policy.h"
#include "lsmkv/metrics.h"
#include "sysinfo.h"
#include "util/random.h"

namespace lsmkv::bench {
namespace {

using Clock = std::chrono::steady_clock;

std::string Key(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%012llu", static_cast<unsigned long long>(i));
  return buf;
}

double ReadCounter(MetricsRegistry* r, const char* name) {
  double v = 0;
  r->GetValue(name, {}, &v);
  return v;
}

struct Row {
  int bits = 0;
  int k = 0;
  double fp_measured = 0;
  double fp_theory = 0;
  double reads_per_negative = 0;
  double reads_per_positive = 0;
  double negative_lookup_us = 0;
  double filter_bytes_per_key = 0;
  int tables_probed = 0;
};

}  // namespace
}  // namespace lsmkv::bench

int main(int argc, char** argv) {
  using namespace lsmkv;
  using namespace lsmkv::bench;
  std::string out;
  std::string dir = "bloom-sweep-db";
  uint64_t records = 400000;
  uint64_t lookups = 100000;
  for (int i = 1; i < argc; i++) {
    if (std::strncmp(argv[i], "--out=", 6) == 0) out = argv[i] + 6;
    if (std::strncmp(argv[i], "--db=", 5) == 0) dir = argv[i] + 5;
    if (std::strncmp(argv[i], "--records=", 10) == 0) records = std::strtoull(argv[i] + 10, nullptr, 10);
    if (std::strncmp(argv[i], "--lookups=", 10) == 0) lookups = std::strtoull(argv[i] + 10, nullptr, 10);
  }
  KeepSystemAwake();
  const SystemInfo sys = CollectSystemInfo(argc, argv);
  std::printf("bloom-sweep  %s | %s\n  %llu records (even keys present, odd keys absent), "
              "%llu lookups each, block cache disabled\n\n",
              sys.cpu_model.c_str(), sys.os.c_str(), static_cast<unsigned long long>(records),
              static_cast<unsigned long long>(lookups));

  std::vector<Row> rows;
  for (int bits : {0, 4, 6, 8, 10, 14}) {
    Row row;
    row.bits = bits;
    // ---- 1. Raw filter false-positive rate ---------------------------------------
    if (bits > 0) {
      auto policy = NewBloomFilterPolicy(bits);
      const int n = 50000;  // about one 2 MB SSTable's worth of keys
      std::vector<std::string> keys;
      for (int i = 0; i < n; i++) keys.push_back(Key(static_cast<uint64_t>(i) * 2));
      std::vector<Slice> slices(keys.begin(), keys.end());
      std::string filter;
      policy->CreateFilter(slices.data(), n, &filter);
      row.k = static_cast<uint8_t>(filter.back());
      row.filter_bytes_per_key = static_cast<double>(filter.size()) / n;
      uint64_t fp = 0;
      const uint64_t probes = 1000000;
      for (uint64_t i = 0; i < probes; i++) {
        if (policy->KeyMayMatch(Key(i * 2 + 1), filter)) fp++;
      }
      row.fp_measured = static_cast<double>(fp) / probes;
      row.fp_theory = std::pow(1.0 - std::exp(-static_cast<double>(row.k) / bits), row.k);
    } else {
      row.fp_measured = row.fp_theory = 1.0;
    }

    // ---- 2. Engine-level reads per lookup ----------------------------------------
    const std::string db_dir = dir + "/bits" + std::to_string(bits);
    (void)DestroyDB(db_dir, Options());
    auto registry = std::make_shared<MetricsRegistry>();
    Options o;
    o.bloom_bits_per_key = bits;
    o.block_cache_capacity = 0;  // every data-block fetch is a file read
    o.write_buffer_size = 1 << 20;
    o.max_file_size = 1 << 20;
    o.max_bytes_for_level_base = 4 << 20;
    o.metrics = registry;
    std::unique_ptr<DB> db;
    Status s = DB::Open(o, db_dir, &db);
    if (!s.ok()) {
      std::fprintf(stderr, "%s\n", s.ToString().c_str());
      return 1;
    }
    Random rnd(99);
    const std::string value(100, 'v');
    // Insert in a random order so L0 files overlap the whole key range.
    std::vector<uint64_t> order(records);
    for (uint64_t i = 0; i < records; i++) order[i] = i;
    for (uint64_t i = records - 1; i > 0; i--) std::swap(order[i], order[rnd.Uniform(i + 1)]);
    for (uint64_t i : order) (void)db->Put(WriteOptions(), Key(i * 2), value);
    (void)db->Flush();
    (void)db->WaitForCompactions();

    for (int level = 0; level < 7; level++) {
      std::string v;
      db->GetProperty("lsmkv.num-files-at-level" + std::to_string(level), &v);
      const int files = std::stoi(v);
      row.tables_probed += level == 0 ? files : (files > 0 ? 1 : 0);
    }

    std::string got;
    const double reads0 = ReadCounter(registry.get(), "lsmkv_sst_block_reads_total");
    const auto t0 = Clock::now();
    for (uint64_t i = 0; i < lookups; i++) {
      (void)db->Get(ReadOptions(), Key(rnd.Uniform(records) * 2 + 1), &got);  // absent
    }
    const auto t1 = Clock::now();
    const double reads1 = ReadCounter(registry.get(), "lsmkv_sst_block_reads_total");
    for (uint64_t i = 0; i < lookups; i++) {
      (void)db->Get(ReadOptions(), Key(rnd.Uniform(records) * 2), &got);  // present
    }
    const double reads2 = ReadCounter(registry.get(), "lsmkv_sst_block_reads_total");
    row.reads_per_negative = (reads1 - reads0) / static_cast<double>(lookups);
    row.reads_per_positive = (reads2 - reads1) / static_cast<double>(lookups);
    row.negative_lookup_us =
        std::chrono::duration<double, std::micro>(t1 - t0).count() / static_cast<double>(lookups);
    db.reset();
    (void)DestroyDB(db_dir, Options());
    rows.push_back(row);
  }

  std::printf("%-9s %-3s %-12s %-12s %-14s %-14s %-12s %s\n", "bits/key", "k", "FP measured",
              "FP theory", "reads/neg get", "reads/pos get", "neg get us", "tables probed");
  for (const Row& r : rows) {
    std::printf("%-9d %-3d %-12.4f %-12.4f %-14.3f %-14.3f %-12.2f %d\n", r.bits, r.k,
                r.fp_measured, r.fp_theory, r.reads_per_negative, r.reads_per_positive,
                r.negative_lookup_us, r.tables_probed);
  }

  if (!out.empty()) {
    std::ostringstream o;
    o << "{\"system\":" << ToJson(sys) << ",\"records\":" << records
      << ",\"lookups\":" << lookups << ",\"rows\":[";
    for (size_t i = 0; i < rows.size(); i++) {
      const Row& r = rows[i];
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    "%s{\"bits_per_key\":%d,\"k\":%d,\"fp_measured\":%.6f,\"fp_theory\":%.6f,"
                    "\"filter_bytes_per_key\":%.3f,\"reads_per_negative_get\":%.4f,"
                    "\"reads_per_positive_get\":%.4f,\"negative_get_us\":%.3f,"
                    "\"tables_probed\":%d}",
                    i ? "," : "", r.bits, r.k, r.fp_measured, r.fp_theory,
                    r.filter_bytes_per_key, r.reads_per_negative, r.reads_per_positive,
                    r.negative_lookup_us, r.tables_probed);
      o << buf;
    }
    o << "]}";
    std::ofstream f(out);
    f << o.str() << "\n";
    std::printf("\nwrote %s\n", out.c_str());
  }
  return 0;
}
