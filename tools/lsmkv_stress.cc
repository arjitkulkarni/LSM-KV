// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// lsmkv-stress: the process that gets killed by tools/crash_matrix.py.
//
//   lsmkv-stress write  --db=DIR [--threads=4] [--keys=4000] [--value_size=120]
//                       [--sync=0|1] [--start=N] [--seed=S] [--write_buffer=B]
//                       [--fail_sync_after=N] [--max_ops=N]
//       Writes forever (or max_ops). After every Put that returns OK it prints
//       "A <key> <counter>" to stdout -- the acknowledgement log. Each thread
//       owns a disjoint key set, so per key the counter only grows.
//
//   lsmkv-stress verify --db=DIR --acked=FILE
//       Opens (recovers) the DB and checks, for every key in the ack log,
//       that the stored value is intact and at least as new as the last
//       acknowledged write. Prints one JSON line; exit 0 iff it holds.

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "db/filename.h"
#include "lsmkv/db.h"
#include "lsmkv/env.h"
#include "util/crc32c.h"
#include "util/fault_injection_env.h"
#include "util/random.h"

namespace lsmkv {
namespace {

struct Flags {
  std::string mode;
  std::string db;
  std::string acked;
  int threads = 4;
  uint64_t keys = 4000;
  size_t value_size = 120;
  bool sync = false;
  uint64_t start = 1;
  uint64_t seed = 1;
  size_t write_buffer = 256 << 10;
  int fail_sync_after = 0;
  uint64_t max_ops = 0;
};

std::string KeyName(uint64_t k) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%08llu", static_cast<unsigned long long>(k));
  return buf;
}

// value := "<key>|<counter>|<crc32c of the two>|" + padding derived from
// the counter. Any torn or mixed-up value fails the checksum.
std::string MakeValue(uint64_t key, uint64_t counter, size_t size) {
  std::string head = std::to_string(key) + "|" + std::to_string(counter) + "|";
  const uint32_t crc = crc32c::Value(head.data(), head.size());
  std::string v = head + std::to_string(crc) + "|";
  while (v.size() < size) v.push_back(static_cast<char>('a' + (counter + v.size()) % 26));
  return v;
}

// Returns false if the value is malformed or fails its checksum.
bool ParseValue(const std::string& v, uint64_t* key, uint64_t* counter) {
  unsigned long long k = 0;
  unsigned long long c = 0;
  unsigned long long crc = 0;
  if (std::sscanf(v.c_str(), "%llu|%llu|%llu|", &k, &c, &crc) != 3) return false;
  const std::string head = std::to_string(k) + "|" + std::to_string(c) + "|";
  if (crc32c::Value(head.data(), head.size()) != crc) return false;
  // The padding is deterministic too: check it so a torn tail is caught.
  const std::string expect = MakeValue(k, c, v.size());
  if (expect != v) return false;
  *key = k;
  *counter = c;
  return true;
}

int RunWrite(const Flags& f) {
  std::unique_ptr<FaultInjectionEnv> fenv;
  Options options;
  options.write_buffer_size = f.write_buffer;
  options.max_file_size = f.write_buffer;
  options.max_bytes_for_level_base = 4 * f.write_buffer;
  if (f.fail_sync_after > 0) {
    fenv = std::make_unique<FaultInjectionEnv>(Env::Default());
    options.env = fenv.get();
  }
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, f.db, &db);
  if (!s.ok()) {
    std::printf("E open %s\n", s.ToString().c_str());
    std::fflush(stdout);
    return 2;
  }
  if (fenv) fenv->FailSyncAfter(fenv->sync_calls() + f.fail_sync_after);

  std::mutex out_mu;
  std::atomic<uint64_t> counter{f.start};
  std::atomic<uint64_t> ops{0};
  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < f.threads; t++) {
    threads.emplace_back([&, t] {
      Random rnd(f.seed * 7919 + static_cast<uint64_t>(t));
      WriteOptions wo;
      wo.sync = f.sync;
      const uint64_t per_thread = f.keys / static_cast<uint64_t>(f.threads);
      while (!failed.load()) {
        if (f.max_ops > 0 && ops.fetch_add(1) >= f.max_ops) break;
        // Thread t owns keys t, t+threads, t+2*threads, ...
        const uint64_t k = rnd.Uniform(per_thread) * static_cast<uint64_t>(f.threads) +
                           static_cast<uint64_t>(t);
        const uint64_t c = counter.fetch_add(1);
        Status ws = db->Put(wo, KeyName(k), MakeValue(k, c, f.value_size));
        std::lock_guard<std::mutex> l(out_mu);
        if (ws.ok()) {
          std::printf("A %llu %llu\n", static_cast<unsigned long long>(k),
                      static_cast<unsigned long long>(c));
        } else {
          std::printf("E %s\n", ws.ToString().c_str());
          failed.store(true);
        }
        std::fflush(stdout);
      }
    });
  }
  for (auto& th : threads) th.join();
  db.reset();
  return failed.load() ? 3 : 0;
}

int RunVerify(const Flags& f) {
  // Last acknowledged counter per key (counters grow per key).
  std::map<uint64_t, uint64_t> acked;
  uint64_t acked_writes = 0;
  {
    std::ifstream in(f.acked);
    std::string tag;
    unsigned long long k;
    unsigned long long c;
    while (in >> tag >> k >> c) {
      if (tag != "A") continue;
      acked_writes++;
      uint64_t& slot = acked[k];
      if (c > slot) slot = c;
    }
  }

  Env* env = Env::Default();
  uint64_t wal_bytes = 0;
  std::vector<std::string> children;
  (void)env->GetChildren(f.db, &children);
  for (const auto& name : children) {
    uint64_t number;
    FileType type;
    if (ParseFileName(name, &number, &type) && type == FileType::kLogFile) {
      uint64_t sz = 0;
      (void)env->GetFileSize(f.db + "/" + name, &sz);
      wal_bytes += sz;
    }
  }

  Options options;
  options.create_if_missing = false;
  options.write_buffer_size = f.write_buffer;
  options.max_file_size = f.write_buffer;
  options.max_bytes_for_level_base = 4 * f.write_buffer;
  const auto t0 = std::chrono::steady_clock::now();
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, f.db, &db);
  const double open_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
          .count();
  if (!s.ok()) {
    std::printf("{\"ok\":false,\"error\":\"open: %s\",\"open_ms\":%.2f,\"wal_bytes\":%llu}\n",
                s.ToString().c_str(), open_ms, static_cast<unsigned long long>(wal_bytes));
    return 1;
  }

  uint64_t lost = 0;
  uint64_t corrupt = 0;
  std::string first_problem;
  for (const auto& [k, c] : acked) {
    std::string v;
    Status g = db->Get(ReadOptions(), KeyName(k), &v);
    uint64_t pk = 0;
    uint64_t pc = 0;
    if (!g.ok()) {
      lost++;
      if (first_problem.empty()) {
        first_problem = "key " + std::to_string(k) + " acked@" + std::to_string(c) +
                        " -> " + g.ToString();
      }
    } else if (!ParseValue(v, &pk, &pc) || pk != k) {
      corrupt++;
      if (first_problem.empty()) first_problem = "key " + std::to_string(k) + " corrupt";
    } else if (pc < c) {
      lost++;  // an older value than the last acknowledged one
      if (first_problem.empty()) {
        first_problem = "key " + std::to_string(k) + " acked@" + std::to_string(c) +
                        " but found @" + std::to_string(pc);
      }
    }
  }

  // Full scan: sorted, and every value well-formed.
  uint64_t scanned = 0;
  auto it = db->NewIterator(ReadOptions());
  std::string prev;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    scanned++;
    const std::string key = it->key().ToString();
    uint64_t pk = 0;
    uint64_t pc = 0;
    if ((!prev.empty() && !(prev < key)) || !ParseValue(it->value().ToString(), &pk, &pc) ||
        KeyName(pk) != key) {
      corrupt++;
      if (first_problem.empty()) first_problem = "scan: bad entry at " + key;
    }
    prev = key;
  }
  if (!it->status().ok()) {
    corrupt++;
    if (first_problem.empty()) first_problem = "scan: " + it->status().ToString();
  }
  it.reset();
  db.reset();

  const bool ok = lost == 0 && corrupt == 0;
  std::printf(
      "{\"ok\":%s,\"acked_writes\":%llu,\"keys_checked\":%zu,\"lost\":%llu,"
      "\"corrupt\":%llu,\"scanned\":%llu,\"open_ms\":%.2f,\"wal_bytes\":%llu,"
      "\"problem\":\"%s\"}\n",
      ok ? "true" : "false", static_cast<unsigned long long>(acked_writes), acked.size(),
      static_cast<unsigned long long>(lost), static_cast<unsigned long long>(corrupt),
      static_cast<unsigned long long>(scanned), open_ms,
      static_cast<unsigned long long>(wal_bytes), first_problem.c_str());
  return ok ? 0 : 1;
}

bool ParseFlag(const char* arg, const char* name, std::string* out) {
  const size_t n = std::strlen(name);
  if (std::strncmp(arg, name, n) == 0 && arg[n] == '=') {
    *out = arg + n + 1;
    return true;
  }
  return false;
}

}  // namespace
}  // namespace lsmkv

int main(int argc, char** argv) {
  using namespace lsmkv;
  if (argc < 2) {
    std::fprintf(stderr, "usage: lsmkv-stress write|verify --db=DIR [...]\n");
    return 64;
  }
  Flags f;
  f.mode = argv[1];
  for (int i = 2; i < argc; i++) {
    std::string v;
    if (ParseFlag(argv[i], "--db", &v)) f.db = v;
    else if (ParseFlag(argv[i], "--acked", &v)) f.acked = v;
    else if (ParseFlag(argv[i], "--threads", &v)) f.threads = std::atoi(v.c_str());
    else if (ParseFlag(argv[i], "--keys", &v)) f.keys = std::strtoull(v.c_str(), nullptr, 10);
    else if (ParseFlag(argv[i], "--value_size", &v)) f.value_size = std::strtoull(v.c_str(), nullptr, 10);
    else if (ParseFlag(argv[i], "--sync", &v)) f.sync = v == "1";
    else if (ParseFlag(argv[i], "--start", &v)) f.start = std::strtoull(v.c_str(), nullptr, 10);
    else if (ParseFlag(argv[i], "--seed", &v)) f.seed = std::strtoull(v.c_str(), nullptr, 10);
    else if (ParseFlag(argv[i], "--write_buffer", &v)) f.write_buffer = std::strtoull(v.c_str(), nullptr, 10);
    else if (ParseFlag(argv[i], "--fail_sync_after", &v)) f.fail_sync_after = std::atoi(v.c_str());
    else if (ParseFlag(argv[i], "--max_ops", &v)) f.max_ops = std::strtoull(v.c_str(), nullptr, 10);
    else {
      std::fprintf(stderr, "unknown flag %s\n", argv[i]);
      return 64;
    }
  }
  if (f.db.empty()) {
    std::fprintf(stderr, "--db is required\n");
    return 64;
  }
  if (f.threads < 1) f.threads = 1;
  if (f.mode == "write") return RunWrite(f);
  if (f.mode == "verify") return RunVerify(f);
  std::fprintf(stderr, "unknown mode %s\n", f.mode.c_str());
  return 64;
}
