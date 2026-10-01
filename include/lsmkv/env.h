// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Env: the engine's only window onto the operating system.

#ifndef LSMKV_INCLUDE_LSMKV_ENV_H_
#define LSMKV_INCLUDE_LSMKV_ENV_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lsmkv/slice.h"
#include "lsmkv/status.h"

namespace lsmkv {

// Sequential reader used for WAL and MANIFEST replay.
class SequentialFile {
 public:
  SequentialFile() = default;
  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;
  virtual ~SequentialFile();

  // Reads up to n bytes; *result may point into scratch[0, n). At EOF the
  // status is OK and *result is empty.
  virtual Status Read(size_t n, Slice* result, char* scratch) = 0;
  virtual Status Skip(uint64_t n) = 0;
};

// Positional reader used for SSTables. Read() must be safe to call from
// many threads concurrently (pread / overlapped ReadFile semantics).
class RandomAccessFile {
 public:
  RandomAccessFile() = default;
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;
  virtual ~RandomAccessFile();

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const = 0;
};

// Append-only writer. The durability ladder is explicit:
//   Append -> user-space buffer (lost on process crash)
//   Flush  -> OS page cache      (survives process crash, not power loss)
//   Sync   -> stable storage     (survives power loss)
class WritableFile {
 public:
  WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;
  virtual ~WritableFile();

  virtual Status Append(const Slice& data) = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};

// An advisory whole-file lock. Destroying the object releases the lock.
class FileLock {
 public:
  FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  virtual ~FileLock();
};

// Abstract OS interface. Production code gets Env::Default(); tests wrap it
// in a FaultInjectionEnv (a decorator) to fail fsyncs or simulate power
// loss without the engine knowing anything changed.
class Env {
 public:
  Env() = default;
  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;
  virtual ~Env();

  // The platform environment (POSIX or Win32). Never destroyed.
  static Env* Default();

  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) = 0;
  virtual Status NewRandomAccessFile(
      const std::string& fname, std::unique_ptr<RandomAccessFile>* result) = 0;
  // Creates or truncates.
  virtual Status NewWritableFile(const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) = 0;
  // Creates or opens for append at the current end of file.
  virtual Status NewAppendableFile(const std::string& fname,
                                   std::unique_ptr<WritableFile>* result) = 0;

  virtual bool FileExists(const std::string& fname) = 0;
  // Returns base names (not paths) of the entries in `dir`.
  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) = 0;
  virtual Status RemoveFile(const std::string& fname) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;  // OK if exists
  virtual Status RemoveDir(const std::string& dirname) = 0;
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;
  // Atomically replaces `target` if it exists.
  virtual Status RenameFile(const std::string& src,
                            const std::string& target) = 0;
  virtual Status TruncateFile(const std::string& fname, uint64_t size) = 0;
  // Makes directory entries (creates/renames) durable. No-op where the
  // filesystem journals metadata synchronously.
  virtual Status SyncDir(const std::string& dirname) = 0;

  virtual Status LockFile(const std::string& fname,
                          std::unique_ptr<FileLock>* lock) = 0;

  virtual uint64_t NowMicros() = 0;
  virtual uint64_t NowNanos() = 0;
  virtual void SleepForMicroseconds(int micros) = 0;
};

// Forwards every call to a target Env. Subclass and override only what you
// want to intercept.
class EnvWrapper : public Env {
 public:
  explicit EnvWrapper(Env* target) : target_(target) {}
  ~EnvWrapper() override;

  Env* target() const { return target_; }

  Status NewSequentialFile(const std::string& f,
                           std::unique_ptr<SequentialFile>* r) override {
    return target_->NewSequentialFile(f, r);
  }
  Status NewRandomAccessFile(const std::string& f,
                             std::unique_ptr<RandomAccessFile>* r) override {
    return target_->NewRandomAccessFile(f, r);
  }
  Status NewWritableFile(const std::string& f,
                         std::unique_ptr<WritableFile>* r) override {
    return target_->NewWritableFile(f, r);
  }
  Status NewAppendableFile(const std::string& f,
                           std::unique_ptr<WritableFile>* r) override {
    return target_->NewAppendableFile(f, r);
  }
  bool FileExists(const std::string& f) override {
    return target_->FileExists(f);
  }
  Status GetChildren(const std::string& d,
                     std::vector<std::string>* r) override {
    return target_->GetChildren(d, r);
  }
  Status RemoveFile(const std::string& f) override {
    return target_->RemoveFile(f);
  }
  Status CreateDir(const std::string& d) override {
    return target_->CreateDir(d);
  }
  Status RemoveDir(const std::string& d) override {
    return target_->RemoveDir(d);
  }
  Status GetFileSize(const std::string& f, uint64_t* s) override {
    return target_->GetFileSize(f, s);
  }
  Status RenameFile(const std::string& s, const std::string& t) override {
    return target_->RenameFile(s, t);
  }
  Status TruncateFile(const std::string& f, uint64_t size) override {
    return target_->TruncateFile(f, size);
  }
  Status SyncDir(const std::string& d) override { return target_->SyncDir(d); }
  Status LockFile(const std::string& f,
                  std::unique_ptr<FileLock>* l) override {
    return target_->LockFile(f, l);
  }
  uint64_t NowMicros() override { return target_->NowMicros(); }
  uint64_t NowNanos() override { return target_->NowNanos(); }
  void SleepForMicroseconds(int m) override {
    target_->SleepForMicroseconds(m);
  }

 private:
  Env* const target_;
};

// Helpers.
Status WriteStringToFile(Env* env, const Slice& data, const std::string& fname,
                         bool sync);
Status ReadFileToString(Env* env, const std::string& fname, std::string* data);

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_ENV_H_
