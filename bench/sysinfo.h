// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Captures the environment a benchmark ran in, so every published number
// carries its hardware, OS, compiler and exact command line.

#ifndef LSMKV_BENCH_SYSINFO_H_
#define LSMKV_BENCH_SYSINFO_H_

#include <string>

namespace lsmkv::bench {

struct SystemInfo {
  std::string cpu_model;
  unsigned hardware_threads = 0;
  unsigned long long memory_bytes = 0;
  std::string os;
  std::string cpu_governor;  // Linux only; "n/a" elsewhere
  std::string compiler;
  std::string build_flags;
  std::string timestamp_utc;
  std::string command_line;
};

SystemInfo CollectSystemInfo(int argc, char** argv);

// Asks the OS not to enter idle sleep while this process runs (Windows:
// SetThreadExecutionState; released automatically at exit). A laptop that
// suspends mid-measurement silently corrupts every latency percentile.
void KeepSystemAwake();

// {"cpu_model": ..., ...}
std::string ToJson(const SystemInfo& info);

// Minimal JSON string escaping for our own output.
std::string JsonEscape(const std::string& s);

}  // namespace lsmkv::bench

#endif  // LSMKV_BENCH_SYSINFO_H_
