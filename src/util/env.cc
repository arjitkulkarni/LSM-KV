// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "lsmkv/env.h"

namespace lsmkv {

SequentialFile::~SequentialFile() = default;
RandomAccessFile::~RandomAccessFile() = default;
WritableFile::~WritableFile() = default;
FileLock::~FileLock() = default;
Env::~Env() = default;
EnvWrapper::~EnvWrapper() = default;

Status WriteStringToFile(Env* env, const Slice& data, const std::string& fname,
                         bool sync) {
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(fname, &file);
  if (!s.ok()) return s;
  s = file->Append(data);
  if (s.ok() && sync) s = file->Sync();
  if (s.ok()) s = file->Close();
  file.reset();
  if (!s.ok()) (void)env->RemoveFile(fname);
  return s;
}

Status ReadFileToString(Env* env, const std::string& fname, std::string* data) {
  data->clear();
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) return s;
  constexpr size_t kBufferSize = 8192;
  std::unique_ptr<char[]> space(new char[kBufferSize]);
  while (true) {
    Slice fragment;
    s = file->Read(kBufferSize, &fragment, space.get());
    if (!s.ok()) break;
    data->append(fragment.data(), fragment.size());
    if (fragment.empty()) break;
  }
  return s;
}

}  // namespace lsmkv
