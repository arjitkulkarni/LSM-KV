// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Directory-level operations shared by the POSIX and Win32 Envs. They go
// through std::filesystem, which already abstracts the platform; only file
// I/O, locking, rename and directory fsync are platform-specific.

#ifndef LSMKV_SRC_UTIL_ENV_COMMON_H_
#define LSMKV_SRC_UTIL_ENV_COMMON_H_

#include <chrono>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "lsmkv/status.h"

namespace lsmkv::env_common {

// OS file locks guard against *other processes*; POSIX fcntl locks do not
// even conflict within one process. This table closes that gap so opening
// the same DB twice in one process fails too.
class LockTable {
 public:
  static LockTable& Instance() {
    static LockTable* table = new LockTable();  // never destroyed
    return *table;
  }
  bool Insert(const std::string& fname) {
    std::lock_guard<std::mutex> l(mu_);
    return locked_.insert(fname).second;
  }
  void Remove(const std::string& fname) {
    std::lock_guard<std::mutex> l(mu_);
    locked_.erase(fname);
  }

 private:
  std::mutex mu_;
  std::set<std::string> locked_;
};

inline Status FromErrorCode(const std::string& context,
                            const std::error_code& ec) {
  if (!ec) return Status::OK();
  if (ec == std::errc::no_such_file_or_directory) {
    return Status::NotFound(context, ec.message());
  }
  return Status::IOError(context, ec.message());
}

inline bool FileExists(const std::string& fname) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::u8path(fname), ec);
}

inline Status GetChildren(const std::string& dir,
                          std::vector<std::string>* result) {
  result->clear();
  std::error_code ec;
  std::filesystem::directory_iterator it(std::filesystem::u8path(dir), ec);
  if (ec) return FromErrorCode(dir, ec);
  for (const auto& entry : it) {
    result->push_back(entry.path().filename().u8string());
  }
  return Status::OK();
}

inline Status RemoveFile(const std::string& fname) {
  std::error_code ec;
  if (!std::filesystem::remove(std::filesystem::u8path(fname), ec) && !ec) {
    return Status::NotFound(fname, "no such file");
  }
  return FromErrorCode(fname, ec);
}

inline Status CreateDir(const std::string& dirname) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(dirname), ec);
  return FromErrorCode(dirname, ec);
}

inline Status RemoveDir(const std::string& dirname) {
  std::error_code ec;
  std::filesystem::remove(std::filesystem::u8path(dirname), ec);
  return FromErrorCode(dirname, ec);
}

inline Status GetFileSize(const std::string& fname, uint64_t* size) {
  std::error_code ec;
  const auto s = std::filesystem::file_size(std::filesystem::u8path(fname), ec);
  if (ec) {
    *size = 0;
    return FromErrorCode(fname, ec);
  }
  *size = static_cast<uint64_t>(s);
  return Status::OK();
}

inline Status TruncateFile(const std::string& fname, uint64_t size) {
  std::error_code ec;
  std::filesystem::resize_file(std::filesystem::u8path(fname), size, ec);
  return FromErrorCode(fname, ec);
}

inline uint64_t NowNanos() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline void SleepForMicroseconds(int micros) {
  std::this_thread::sleep_for(std::chrono::microseconds(micros));
}

}  // namespace lsmkv::env_common

#endif  // LSMKV_SRC_UTIL_ENV_COMMON_H_
