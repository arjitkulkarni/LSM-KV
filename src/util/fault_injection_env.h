// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// FaultInjectionEnv: an Env decorator for crash and I/O-failure testing.

#ifndef LSMKV_SRC_UTIL_FAULT_INJECTION_ENV_H_
#define LSMKV_SRC_UTIL_FAULT_INJECTION_ENV_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "lsmkv/env.h"

namespace lsmkv {

// Wraps a real Env and tracks, for every file written through it, how many
// bytes were appended and how many of those were covered by a successful
// Sync(). That is enough to model the two failures that matter:
//
//  * Power loss: DropUnsyncedData() truncates every file to its last synced
//    length -- exactly what a machine that lost power before the page cache
//    was written back would show on reboot. The engine must reopen with
//    every write acked under WriteOptions::sync = true intact.
//
//  * Failed fsync: FailSyncAfter(n) makes the n-th and every later Sync()
//    return an IOError. The engine must stop acknowledging writes (it may
//    not retry and pretend success -- the kernel may already have dropped
//    the dirty pages, the "fsyncgate" lesson from PostgreSQL).
//
// Not modelled: loss of un-synced directory entries (creates/renames).
class FaultInjectionEnv : public EnvWrapper {
 public:
  explicit FaultInjectionEnv(Env* base) : EnvWrapper(base) {}

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override;
  Status NewAppendableFile(const std::string& fname,
                           std::unique_ptr<WritableFile>* result) override;
  Status RemoveFile(const std::string& fname) override;
  Status RenameFile(const std::string& src, const std::string& target) override;

  // 0 disables injection. Otherwise the n-th Sync() (1-based) and all later
  // ones fail.
  void FailSyncAfter(int n) { fail_sync_after_.store(n); }
  int sync_calls() const { return sync_calls_.load(); }

  // While inactive, every Append/Sync fails: models the instant of a crash.
  void SetFilesystemActive(bool active) { active_.store(active); }
  bool IsFilesystemActive() const { return active_.load(); }

  // Truncates every tracked file to the length covered by its last
  // successful Sync(). Call only after the DB has been closed.
  Status DropUnsyncedData();

  // Forget all tracked state (e.g. after DropUnsyncedData + reopen).
  void ResetState();

 private:
  friend class TestWritableFile;
  struct FileState {
    uint64_t pos = 0;
    uint64_t synced = 0;
  };

  void OnAppend(const std::string& fname, uint64_t n);
  void OnSync(const std::string& fname, uint64_t pos_at_sync);
  bool ShouldFailSync();

  std::mutex mu_;
  std::map<std::string, FileState> files_;
  std::atomic<int> fail_sync_after_{0};
  std::atomic<int> sync_calls_{0};
  std::atomic<bool> active_{true};
};

}  // namespace lsmkv

#endif  // LSMKV_SRC_UTIL_FAULT_INJECTION_ENV_H_
