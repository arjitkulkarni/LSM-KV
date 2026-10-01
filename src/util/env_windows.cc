// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Win32 Env. Mirrors env_posix.cc:
//   * positional reads via ReadFile + OVERLAPPED.Offset (thread-safe, the
//     Win32 equivalent of pread),
//   * FlushFileBuffers for Sync (the equivalent of fsync),
//   * MoveFileEx(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) for
//     atomic rename-over,
//   * every handle opened with FILE_SHARE_DELETE so an SSTable that a
//     reader still has open can be deleted by compaction (POSIX unlink
//     semantics).

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <new>

#include "lsmkv/env.h"
#include "util/env_common.h"

namespace lsmkv {
namespace {

constexpr size_t kWritableFileBufferSize = 64 * 1024;

std::string LastErrorMessage(DWORD err) {
  char* buf = nullptr;
  const DWORD n = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&buf), 0, nullptr);
  std::string msg = (n > 0 && buf != nullptr) ? std::string(buf, n)
                                              : "error " + std::to_string(err);
  if (buf != nullptr) ::LocalFree(buf);
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
    msg.pop_back();
  }
  return msg;
}

// Anti-virus and search indexers briefly open freshly written files, which
// makes MoveFileEx / DeleteFile fail with ACCESS_DENIED or
// SHARING_VIOLATION for a few milliseconds. Retry those (only those) with a
// short backoff before reporting an error.
template <typename Fn>
bool RetryTransientSharingErrors(Fn fn) {
  for (int attempt = 0;; attempt++) {
    if (fn()) return true;
    const DWORD err = ::GetLastError();
    if ((err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) ||
        attempt >= 30) {
      ::SetLastError(err);
      return false;
    }
    ::Sleep(attempt < 5 ? 1 : 10);
  }
}

Status WindowsError(const std::string& context, DWORD err) {
  if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
    return Status::NotFound(context, LastErrorMessage(err));
  }
  return Status::IOError(context, LastErrorMessage(err));
}

// RAII owner of a Win32 HANDLE.
class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE h) : h_(h) {}
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ~ScopedHandle() { Close(); }
  HANDLE get() const { return h_; }
  bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
  bool Close() {
    if (!valid()) return true;
    const bool ok = ::CloseHandle(h_) != 0;
    h_ = INVALID_HANDLE_VALUE;
    return ok;
  }

 private:
  HANDLE h_;
};

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

class WindowsSequentialFile final : public SequentialFile {
 public:
  WindowsSequentialFile(std::string filename, HANDLE h)
      : handle_(h), filename_(std::move(filename)) {}

  Status Read(size_t n, Slice* result, char* scratch) override {
    DWORD bytes_read = 0;
    const auto to_read = static_cast<DWORD>(std::min<size_t>(n, MAXDWORD));
    if (!::ReadFile(handle_.get(), scratch, to_read, &bytes_read, nullptr)) {
      return WindowsError(filename_, ::GetLastError());
    }
    *result = Slice(scratch, bytes_read);
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    LARGE_INTEGER distance;
    distance.QuadPart = static_cast<LONGLONG>(n);
    if (!::SetFilePointerEx(handle_.get(), distance, nullptr, FILE_CURRENT)) {
      return WindowsError(filename_, ::GetLastError());
    }
    return Status::OK();
  }

 private:
  ScopedHandle handle_;
  const std::string filename_;
};

class WindowsRandomAccessFile final : public RandomAccessFile {
 public:
  WindowsRandomAccessFile(std::string filename, HANDLE h)
      : handle_(h), filename_(std::move(filename)) {}

  Status Read(uint64_t offset, size_t n, Slice* result,
              char* scratch) const override {
    size_t done = 0;
    while (done < n) {
      OVERLAPPED overlapped{};
      const uint64_t pos = offset + done;
      overlapped.OffsetHigh = static_cast<DWORD>(pos >> 32);
      overlapped.Offset = static_cast<DWORD>(pos);
      DWORD bytes_read = 0;
      const auto to_read = static_cast<DWORD>(std::min<size_t>(n - done, MAXDWORD));
      if (!::ReadFile(handle_.get(), scratch + done, to_read, &bytes_read,
                      &overlapped)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_HANDLE_EOF) break;
        *result = Slice(scratch, done);
        return WindowsError(filename_, err);
      }
      if (bytes_read == 0) break;
      done += bytes_read;
    }
    *result = Slice(scratch, done);
    return Status::OK();
  }

 private:
  ScopedHandle handle_;
  const std::string filename_;
};

class WindowsWritableFile final : public WritableFile {
 public:
  WindowsWritableFile(std::string filename, HANDLE h)
      : handle_(h), filename_(std::move(filename)) {}
  ~WindowsWritableFile() override {
    if (handle_.valid()) (void)Close();
  }

  Status Append(const Slice& data) override {
    const char* p = data.data();
    size_t n = data.size();
    const size_t copy = std::min(n, kWritableFileBufferSize - pos_);
    std::memcpy(buf_ + pos_, p, copy);
    p += copy;
    n -= copy;
    pos_ += copy;
    if (n == 0) return Status::OK();

    Status s = FlushBuffer();
    if (!s.ok()) return s;
    if (n < kWritableFileBufferSize) {
      std::memcpy(buf_, p, n);
      pos_ = n;
      return Status::OK();
    }
    return WriteUnbuffered(p, n);
  }

  Status Flush() override { return FlushBuffer(); }

  Status Sync() override {
    Status s = FlushBuffer();
    if (!s.ok()) return s;
    if (!::FlushFileBuffers(handle_.get())) {
      return WindowsError(filename_, ::GetLastError());
    }
    return Status::OK();
  }

  Status Close() override {
    Status s = FlushBuffer();
    if (!handle_.Close() && s.ok()) {
      s = WindowsError(filename_, ::GetLastError());
    }
    return s;
  }

 private:
  Status FlushBuffer() {
    Status s = WriteUnbuffered(buf_, pos_);
    pos_ = 0;
    return s;
  }

  Status WriteUnbuffered(const char* data, size_t size) {
    while (size > 0) {
      DWORD written = 0;
      const auto chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30));
      if (!::WriteFile(handle_.get(), data, chunk, &written, nullptr)) {
        return WindowsError(filename_, ::GetLastError());
      }
      data += written;
      size -= written;
    }
    return Status::OK();
  }

  char buf_[kWritableFileBufferSize];
  size_t pos_ = 0;
  ScopedHandle handle_;
  const std::string filename_;
};

class WindowsFileLock final : public FileLock {
 public:
  WindowsFileLock(HANDLE h, std::string fname)
      : handle_(h), fname_(std::move(fname)) {}
  ~WindowsFileLock() override {
    OVERLAPPED ov{};
    ::UnlockFileEx(handle_.get(), 0, MAXDWORD, MAXDWORD, &ov);
    handle_.Close();
    env_common::LockTable::Instance().Remove(fname_);
  }

 private:
  ScopedHandle handle_;
  std::string fname_;
};

class WindowsEnv final : public Env {
 public:
  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    HANDLE h = ::CreateFileA(fname.c_str(), GENERIC_READ, kShareAll, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                             FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return WindowsError(fname, ::GetLastError());
    *result = std::make_unique<WindowsSequentialFile>(fname, h);
    return Status::OK();
  }

  Status NewRandomAccessFile(
      const std::string& fname,
      std::unique_ptr<RandomAccessFile>* result) override {
    HANDLE h = ::CreateFileA(fname.c_str(), GENERIC_READ, kShareAll, nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) return WindowsError(fname, ::GetLastError());
    *result = std::make_unique<WindowsRandomAccessFile>(fname, h);
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    HANDLE h = ::CreateFileA(fname.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return WindowsError(fname, ::GetLastError());
    *result = std::make_unique<WindowsWritableFile>(fname, h);
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& fname,
                           std::unique_ptr<WritableFile>* result) override {
    HANDLE h = ::CreateFileA(fname.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return WindowsError(fname, ::GetLastError());
    *result = std::make_unique<WindowsWritableFile>(fname, h);
    return Status::OK();
  }

  bool FileExists(const std::string& f) override {
    return env_common::FileExists(f);
  }
  Status GetChildren(const std::string& d,
                     std::vector<std::string>* r) override {
    return env_common::GetChildren(d, r);
  }
  Status RemoveFile(const std::string& f) override {
    if (!RetryTransientSharingErrors([&] { return ::DeleteFileA(f.c_str()) != 0; })) {
      return WindowsError(f, ::GetLastError());
    }
    return Status::OK();
  }
  Status CreateDir(const std::string& d) override {
    return env_common::CreateDir(d);
  }
  Status RemoveDir(const std::string& d) override {
    return env_common::RemoveDir(d);
  }
  Status GetFileSize(const std::string& f, uint64_t* s) override {
    return env_common::GetFileSize(f, s);
  }
  Status TruncateFile(const std::string& f, uint64_t size) override {
    return env_common::TruncateFile(f, size);
  }

  Status RenameFile(const std::string& src,
                    const std::string& target) override {
    const bool ok = RetryTransientSharingErrors([&] {
      return ::MoveFileExA(src.c_str(), target.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    });
    if (!ok) return WindowsError(src, ::GetLastError());
    return Status::OK();
  }

  // NTFS journals directory metadata; MOVEFILE_WRITE_THROUGH already makes
  // the rename itself durable before MoveFileEx returns.
  Status SyncDir(const std::string&) override { return Status::OK(); }

  Status LockFile(const std::string& fname,
                  std::unique_ptr<FileLock>* lock) override {
    if (!env_common::LockTable::Instance().Insert(fname)) {
      return Status::IOError("lock " + fname, "already held by this process");
    }
    HANDLE h = ::CreateFileA(fname.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      const DWORD err = ::GetLastError();
      env_common::LockTable::Instance().Remove(fname);
      return WindowsError("lock " + fname + " (held by another process?)", err);
    }
    OVERLAPPED ov{};
    if (!::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                      0, MAXDWORD, MAXDWORD, &ov)) {
      const DWORD err = ::GetLastError();
      ::CloseHandle(h);
      env_common::LockTable::Instance().Remove(fname);
      return WindowsError("lock " + fname, err);
    }
    *lock = std::make_unique<WindowsFileLock>(h, fname);
    return Status::OK();
  }

  uint64_t NowMicros() override { return env_common::NowNanos() / 1000; }
  uint64_t NowNanos() override { return env_common::NowNanos(); }
  void SleepForMicroseconds(int m) override {
    env_common::SleepForMicroseconds(m);
  }
};

}  // namespace

Env* Env::Default() {
  alignas(WindowsEnv) static unsigned char storage[sizeof(WindowsEnv)];
  static Env* const env = new (storage) WindowsEnv();
  return env;
}

}  // namespace lsmkv

#endif  // defined(_WIN32)
