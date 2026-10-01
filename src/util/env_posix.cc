// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// POSIX Env: pread for concurrent positional reads, a user-space write
// buffer in front of write(2), fdatasync for durability, fcntl for locking.

#if !defined(_WIN32)

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>

#include "lsmkv/env.h"
#include "util/env_common.h"

namespace lsmkv {
namespace {

constexpr size_t kWritableFileBufferSize = 64 * 1024;

Status PosixError(const std::string& context, int error_number) {
  if (error_number == ENOENT) {
    return Status::NotFound(context, std::strerror(error_number));
  }
  return Status::IOError(context, std::strerror(error_number));
}

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(std::string filename, int fd)
      : fd_(fd), filename_(std::move(filename)) {}
  ~PosixSequentialFile() override { ::close(fd_); }

  Status Read(size_t n, Slice* result, char* scratch) override {
    while (true) {
      const ::ssize_t r = ::read(fd_, scratch, n);
      if (r < 0) {
        if (errno == EINTR) continue;
        return PosixError(filename_, errno);
      }
      *result = Slice(scratch, static_cast<size_t>(r));
      return Status::OK();
    }
  }

  Status Skip(uint64_t n) override {
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) == static_cast<off_t>(-1)) {
      return PosixError(filename_, errno);
    }
    return Status::OK();
  }

 private:
  const int fd_;
  const std::string filename_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string filename, int fd)
      : fd_(fd), filename_(std::move(filename)) {}
  ~PosixRandomAccessFile() override { ::close(fd_); }

  Status Read(uint64_t offset, size_t n, Slice* result,
              char* scratch) const override {
    size_t done = 0;
    while (done < n) {
      const ::ssize_t r = ::pread(fd_, scratch + done, n - done,
                                  static_cast<off_t>(offset + done));
      if (r < 0) {
        if (errno == EINTR) continue;
        *result = Slice(scratch, done);
        return PosixError(filename_, errno);
      }
      if (r == 0) break;  // EOF
      done += static_cast<size_t>(r);
    }
    *result = Slice(scratch, done);
    return Status::OK();
  }

 private:
  const int fd_;
  const std::string filename_;
};

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string filename, int fd)
      : fd_(fd), filename_(std::move(filename)) {}
  ~PosixWritableFile() override {
    if (fd_ >= 0) (void)Close();
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
#if defined(__APPLE__)
    if (::fcntl(fd_, F_FULLFSYNC) == 0) return Status::OK();
#endif
#if defined(__linux__)
    if (::fdatasync(fd_) != 0) return PosixError(filename_, errno);
#else
    if (::fsync(fd_) != 0) return PosixError(filename_, errno);
#endif
    return Status::OK();
  }

  Status Close() override {
    Status s = FlushBuffer();
    if (::close(fd_) < 0 && s.ok()) s = PosixError(filename_, errno);
    fd_ = -1;
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
      const ::ssize_t r = ::write(fd_, data, size);
      if (r < 0) {
        if (errno == EINTR) continue;
        return PosixError(filename_, errno);
      }
      data += r;
      size -= static_cast<size_t>(r);
    }
    return Status::OK();
  }

  char buf_[kWritableFileBufferSize];
  size_t pos_ = 0;
  int fd_;
  const std::string filename_;
};

class PosixFileLock final : public FileLock {
 public:
  PosixFileLock(int fd, std::string fname) : fd_(fd), fname_(std::move(fname)) {}
  ~PosixFileLock() override {
    struct ::flock f {};
    f.l_type = F_UNLCK;
    f.l_whence = SEEK_SET;
    (void)::fcntl(fd_, F_SETLK, &f);
    ::close(fd_);
    env_common::LockTable::Instance().Remove(fname_);
  }

 private:
  int fd_;
  std::string fname_;
};

class PosixEnv final : public Env {
 public:
  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(fname, errno);
    *result = std::make_unique<PosixSequentialFile>(fname, fd);
    return Status::OK();
  }

  Status NewRandomAccessFile(
      const std::string& fname,
      std::unique_ptr<RandomAccessFile>* result) override {
    const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(fname, errno);
    *result = std::make_unique<PosixRandomAccessFile>(fname, fd);
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    const int fd =
        ::open(fname.c_str(), O_TRUNC | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(fname, errno);
    *result = std::make_unique<PosixWritableFile>(fname, fd);
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& fname,
                           std::unique_ptr<WritableFile>* result) override {
    const int fd =
        ::open(fname.c_str(), O_APPEND | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(fname, errno);
    *result = std::make_unique<PosixWritableFile>(fname, fd);
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
    return env_common::RemoveFile(f);
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
    if (::rename(src.c_str(), target.c_str()) != 0) {
      return PosixError(src, errno);
    }
    return Status::OK();
  }

  Status SyncDir(const std::string& dirname) override {
    const int fd = ::open(dirname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(dirname, errno);
    Status s;
    if (::fsync(fd) != 0) s = PosixError(dirname, errno);
    ::close(fd);
    return s;
  }

  Status LockFile(const std::string& fname,
                  std::unique_ptr<FileLock>* lock) override {
    if (!env_common::LockTable::Instance().Insert(fname)) {
      return Status::IOError("lock " + fname, "already held by this process");
    }
    const int fd = ::open(fname.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      const int err = errno;
      env_common::LockTable::Instance().Remove(fname);
      return PosixError(fname, err);
    }
    struct ::flock f {};
    f.l_type = F_WRLCK;
    f.l_whence = SEEK_SET;
    if (::fcntl(fd, F_SETLK, &f) == -1) {
      const int err = errno;
      ::close(fd);
      env_common::LockTable::Instance().Remove(fname);
      return PosixError("lock " + fname + " (held by another process?)", err);
    }
    *lock = std::make_unique<PosixFileLock>(fd, fname);
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
  // Constructed on first use and intentionally never destroyed, so it stays
  // usable from other static destructors and detached threads.
  alignas(PosixEnv) static unsigned char storage[sizeof(PosixEnv)];
  static Env* const env = new (storage) PosixEnv();
  return env;
}

}  // namespace lsmkv

#endif  // !defined(_WIN32)
