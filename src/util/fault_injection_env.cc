// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "util/fault_injection_env.h"

#include <vector>

namespace lsmkv {

class TestWritableFile final : public WritableFile {
 public:
  TestWritableFile(FaultInjectionEnv* env, std::string fname,
                   std::unique_ptr<WritableFile> target, uint64_t start_pos)
      : env_(env),
        fname_(std::move(fname)),
        target_(std::move(target)),
        pos_(start_pos) {}

  Status Append(const Slice& data) override {
    if (!env_->IsFilesystemActive()) {
      return Status::IOError(fname_, "filesystem inactive (simulated crash)");
    }
    Status s = target_->Append(data);
    if (s.ok()) {
      pos_ += data.size();
      env_->OnAppend(fname_, data.size());
    }
    return s;
  }

  Status Flush() override {
    if (!env_->IsFilesystemActive()) {
      return Status::IOError(fname_, "filesystem inactive (simulated crash)");
    }
    return target_->Flush();
  }

  Status Sync() override {
    if (!env_->IsFilesystemActive()) {
      return Status::IOError(fname_, "filesystem inactive (simulated crash)");
    }
    if (env_->ShouldFailSync()) {
      // The data stays in the OS buffer but is never marked durable, so a
      // later DropUnsyncedData() discards it -- as a real kernel may.
      (void)target_->Flush();
      return Status::IOError(fname_, "injected fsync failure");
    }
    Status s = target_->Sync();
    if (s.ok()) env_->OnSync(fname_, pos_);
    return s;
  }

  Status Close() override { return target_->Close(); }

 private:
  FaultInjectionEnv* const env_;
  const std::string fname_;
  std::unique_ptr<WritableFile> target_;
  uint64_t pos_;
};

Status FaultInjectionEnv::NewWritableFile(
    const std::string& fname, std::unique_ptr<WritableFile>* result) {
  std::unique_ptr<WritableFile> base;
  Status s = target()->NewWritableFile(fname, &base);
  if (!s.ok()) return s;
  {
    std::lock_guard<std::mutex> l(mu_);
    files_[fname] = FileState{};  // created empty, nothing durable yet
  }
  *result = std::make_unique<TestWritableFile>(this, fname, std::move(base), 0);
  return Status::OK();
}

Status FaultInjectionEnv::NewAppendableFile(
    const std::string& fname, std::unique_ptr<WritableFile>* result) {
  uint64_t size = 0;
  if (target()->FileExists(fname)) {
    Status s = target()->GetFileSize(fname, &size);
    if (!s.ok()) return s;
  }
  std::unique_ptr<WritableFile> base;
  Status s = target()->NewAppendableFile(fname, &base);
  if (!s.ok()) return s;
  {
    std::lock_guard<std::mutex> l(mu_);
    auto it = files_.find(fname);
    if (it == files_.end()) {
      // Pre-existing content is assumed durable.
      files_[fname] = FileState{size, size};
    }
  }
  *result =
      std::make_unique<TestWritableFile>(this, fname, std::move(base), size);
  return Status::OK();
}

Status FaultInjectionEnv::RemoveFile(const std::string& fname) {
  Status s = target()->RemoveFile(fname);
  if (s.ok()) {
    std::lock_guard<std::mutex> l(mu_);
    files_.erase(fname);
  }
  return s;
}

Status FaultInjectionEnv::RenameFile(const std::string& src,
                                     const std::string& target_name) {
  Status s = target()->RenameFile(src, target_name);
  if (s.ok()) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = files_.find(src);
    if (it != files_.end()) {
      files_[target_name] = it->second;
      files_.erase(src);
    } else {
      files_.erase(target_name);
    }
  }
  return s;
}

void FaultInjectionEnv::OnAppend(const std::string& fname, uint64_t n) {
  std::lock_guard<std::mutex> l(mu_);
  files_[fname].pos += n;
}

void FaultInjectionEnv::OnSync(const std::string& fname, uint64_t pos_at_sync) {
  std::lock_guard<std::mutex> l(mu_);
  FileState& st = files_[fname];
  if (pos_at_sync > st.synced) st.synced = pos_at_sync;
}

bool FaultInjectionEnv::ShouldFailSync() {
  const int n = sync_calls_.fetch_add(1) + 1;
  const int limit = fail_sync_after_.load();
  return limit > 0 && n >= limit;
}

Status FaultInjectionEnv::DropUnsyncedData() {
  std::vector<std::pair<std::string, uint64_t>> work;
  {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& [name, st] : files_) {
      if (st.pos > st.synced) work.emplace_back(name, st.synced);
      st.pos = st.synced;
    }
  }
  for (const auto& [name, size] : work) {
    if (!target()->FileExists(name)) continue;
    Status s = target()->TruncateFile(name, size);
    if (!s.ok()) return s;
  }
  return Status::OK();
}

void FaultInjectionEnv::ResetState() {
  std::lock_guard<std::mutex> l(mu_);
  files_.clear();
  sync_calls_.store(0);
  fail_sync_after_.store(0);
  active_.store(true);
}

}  // namespace lsmkv
