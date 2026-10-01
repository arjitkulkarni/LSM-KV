// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "db/filename.h"

#include <cctype>
#include <cstdio>

#include "lsmkv/env.h"

namespace lsmkv {

namespace {

std::string MakeFileName(const std::string& dbname, uint64_t number,
                         const char* suffix) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s",
                static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

// Parses a decimal number from the front of *s. Returns false on overflow
// or if no digit is present.
bool ConsumeDecimalNumber(std::string* s, uint64_t* val) {
  constexpr uint64_t kMax = ~uint64_t{0};
  uint64_t v = 0;
  size_t i = 0;
  for (; i < s->size() && std::isdigit(static_cast<unsigned char>((*s)[i]));
       i++) {
    const uint64_t digit = static_cast<uint64_t>((*s)[i] - '0');
    if (v > (kMax - digit) / 10) return false;
    v = v * 10 + digit;
  }
  if (i == 0) return false;
  s->erase(0, i);
  *val = v;
  return true;
}

}  // namespace

std::string LogFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "log");
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "sst");
}

std::string DescriptorFileName(const std::string& dbname, uint64_t number) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu",
                static_cast<unsigned long long>(number));
  return dbname + buf;
}

std::string CurrentFileName(const std::string& dbname) {
  return dbname + "/CURRENT";
}

std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

std::string TempFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "dbtmp");
}

std::string InfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG";
}

std::string OldInfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG.old";
}

bool ParseFileName(const std::string& filename, uint64_t* number,
                   FileType* type) {
  std::string rest = filename;
  if (rest == "CURRENT") {
    *number = 0;
    *type = FileType::kCurrentFile;
  } else if (rest == "LOCK") {
    *number = 0;
    *type = FileType::kDBLockFile;
  } else if (rest == "LOG" || rest == "LOG.old") {
    *number = 0;
    *type = FileType::kInfoLogFile;
  } else if (rest.rfind("MANIFEST-", 0) == 0) {
    rest.erase(0, 9);
    uint64_t num;
    if (!ConsumeDecimalNumber(&rest, &num) || !rest.empty()) return false;
    *type = FileType::kDescriptorFile;
    *number = num;
  } else {
    uint64_t num;
    if (!ConsumeDecimalNumber(&rest, &num)) return false;
    if (rest == ".log") {
      *type = FileType::kLogFile;
    } else if (rest == ".sst") {
      *type = FileType::kTableFile;
    } else if (rest == ".dbtmp") {
      *type = FileType::kTempFile;
    } else {
      return false;
    }
    *number = num;
  }
  return true;
}

Status SetCurrentFile(Env* env, const std::string& dbname,
                      uint64_t descriptor_number) {
  // Remove the leading "dbname/" and add a newline to the manifest name.
  std::string manifest = DescriptorFileName(dbname, descriptor_number);
  const std::string contents = manifest.substr(dbname.size() + 1) + "\n";
  const std::string tmp = TempFileName(dbname, descriptor_number);
  Status s = WriteStringToFile(env, contents, tmp, /*sync=*/true);
  if (s.ok()) s = env->RenameFile(tmp, CurrentFileName(dbname));
  if (s.ok()) s = env->SyncDir(dbname);
  if (!s.ok()) (void)env->RemoveFile(tmp);
  return s;
}

}  // namespace lsmkv
