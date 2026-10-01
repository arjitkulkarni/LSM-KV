// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "db/filename.h"

namespace lsmkv::test {

namespace fs = std::filesystem;

TempDir::TempDir(const std::string& tag) {
  static std::atomic<int> counter{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  Random rnd(static_cast<uint64_t>(now));
  const fs::path p = fs::temp_directory_path() /
                     (tag + "-" + std::to_string(counter.fetch_add(1)) + "-" +
                      std::to_string(rnd.Next() % 1000000000));
  fs::create_directories(p);
  path_ = p.u8string();
  std::replace(path_.begin(), path_.end(), '\\', '/');
}

TempDir::~TempDir() {
  std::error_code ec;
  fs::remove_all(fs::u8path(path_), ec);
}

std::string RandomString(Random* rnd, size_t len) {
  std::string s(len, '\0');
  for (size_t i = 0; i < len; i++) {
    s[i] = static_cast<char>(' ' + rnd->Uniform(95));  // printable
  }
  return s;
}

std::string Key(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%010llu",
                static_cast<unsigned long long>(i));
  return buf;
}

std::vector<uint64_t> ListFiles(const std::string& dir,
                                const std::string& suffix) {
  std::vector<uint64_t> out;
  for (const auto& entry : fs::directory_iterator(fs::u8path(dir))) {
    const std::string name = entry.path().filename().u8string();
    uint64_t number;
    FileType type;
    if (!ParseFileName(name, &number, &type)) continue;
    if ((suffix == ".log" && type == FileType::kLogFile) ||
        (suffix == ".sst" && type == FileType::kTableFile)) {
      out.push_back(number);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string FileNameFor(const std::string& dir, uint64_t number,
                        const std::string& suffix) {
  return suffix == ".log" ? LogFileName(dir, number) : TableFileName(dir, number);
}

void CorruptByte(const std::string& fname, uint64_t offset, int delta) {
  std::fstream f(fs::u8path(fname), std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(f.good()) << fname;
  f.seekg(static_cast<std::streamoff>(offset));
  char c = 0;
  f.read(&c, 1);
  c = static_cast<char>(c + delta);
  f.seekp(static_cast<std::streamoff>(offset));
  f.write(&c, 1);
}

void TruncateBy(const std::string& fname, uint64_t bytes) {
  const uint64_t size = FileSize(fname);
  fs::resize_file(fs::u8path(fname), size > bytes ? size - bytes : 0);
}

void AppendBytes(const std::string& fname, const std::string& bytes) {
  std::ofstream f(fs::u8path(fname), std::ios::app | std::ios::binary);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

uint64_t FileSize(const std::string& fname) {
  return static_cast<uint64_t>(fs::file_size(fs::u8path(fname)));
}

Status StringSource::Read(size_t n, Slice* result, char* scratch) {
  const size_t avail = contents_.size() - pos_;
  n = std::min(n, avail);
  std::memcpy(scratch, contents_.data() + pos_, n);
  pos_ += n;
  *result = Slice(scratch, n);
  return Status::OK();
}

Status StringSource::Skip(uint64_t n) {
  pos_ = std::min<size_t>(contents_.size(), pos_ + static_cast<size_t>(n));
  return Status::OK();
}

Status StringSource::Read(uint64_t offset, size_t n, Slice* result,
                          char* scratch) const {
  if (offset > contents_.size()) {
    return Status::InvalidArgument("invalid read offset");
  }
  n = std::min<size_t>(n, contents_.size() - static_cast<size_t>(offset));
  std::memcpy(scratch, contents_.data() + offset, n);
  *result = Slice(scratch, n);
  return Status::OK();
}

std::unique_ptr<DB> OpenOrDie(const Options& options, const std::string& path) {
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, path, &db);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return db;
}

}  // namespace lsmkv::test
