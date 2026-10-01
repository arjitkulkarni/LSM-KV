// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_TESTS_TEST_UTIL_H_
#define LSMKV_TESTS_TEST_UTIL_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lsmkv/db.h"
#include "lsmkv/env.h"
#include "util/random.h"

namespace lsmkv::test {

// A unique scratch directory under the system temp dir; removed (with its
// contents) on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& tag = "lsmkv");
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir();

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

std::string RandomString(Random* rnd, size_t len);
// Zero-padded so lexical order == numeric order: Key(7) == "key0000000007".
std::string Key(uint64_t i);

// Returns the numbers of files of the given suffix (".log", ".sst") in dir,
// sorted ascending.
std::vector<uint64_t> ListFiles(const std::string& dir, const std::string& suffix);
std::string FileNameFor(const std::string& dir, uint64_t number,
                        const std::string& suffix);

// Byte-level file surgery for corruption tests.
void CorruptByte(const std::string& fname, uint64_t offset, int delta = 0x41);
void TruncateBy(const std::string& fname, uint64_t bytes);
void AppendBytes(const std::string& fname, const std::string& bytes);
uint64_t FileSize(const std::string& fname);

// In-memory files, so format-level tests never touch the disk.
class StringSink final : public WritableFile {
 public:
  Status Append(const Slice& data) override {
    contents_.append(data.data(), data.size());
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }
  std::string& contents() { return contents_; }

 private:
  std::string contents_;
};

class StringSource final : public SequentialFile, public RandomAccessFile {
 public:
  explicit StringSource(std::string contents) : contents_(std::move(contents)) {}

  Status Read(size_t n, Slice* result, char* scratch) override;
  Status Skip(uint64_t n) override;
  Status Read(uint64_t offset, size_t n, Slice* result,
              char* scratch) const override;
  std::string& contents() { return contents_; }

 private:
  std::string contents_;
  size_t pos_ = 0;
};

// Opens `path` with `options`, failing the current test on error.
std::unique_ptr<DB> OpenOrDie(const Options& options, const std::string& path);

}  // namespace lsmkv::test

#endif  // LSMKV_TESTS_TEST_UTIL_H_
