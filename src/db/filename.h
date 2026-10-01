// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Names of the files that make up a database directory:
//
//   000012.log        write-ahead log segment
//   000045.sst        SSTable
//   MANIFEST-000007   log of VersionEdits: the authoritative live-file set
//   CURRENT           names the active MANIFEST (replaced atomically)
//   LOCK              held while the DB is open (one process at a time)
//   LOG / LOG.old     structured JSON event log
//   000046.dbtmp      temporary file (CURRENT being rewritten)

#ifndef LSMKV_SRC_DB_FILENAME_H_
#define LSMKV_SRC_DB_FILENAME_H_

#include <cstdint>
#include <string>

#include "lsmkv/status.h"

namespace lsmkv {

class Env;

enum class FileType {
  kLogFile,
  kDBLockFile,
  kTableFile,
  kDescriptorFile,
  kCurrentFile,
  kTempFile,
  kInfoLogFile,
};

std::string LogFileName(const std::string& dbname, uint64_t number);
std::string TableFileName(const std::string& dbname, uint64_t number);
std::string DescriptorFileName(const std::string& dbname, uint64_t number);
std::string CurrentFileName(const std::string& dbname);
std::string LockFileName(const std::string& dbname);
std::string TempFileName(const std::string& dbname, uint64_t number);
std::string InfoLogFileName(const std::string& dbname);
std::string OldInfoLogFileName(const std::string& dbname);

// If `filename` is a file this engine created, stores its type and number
// and returns true.
bool ParseFileName(const std::string& filename, uint64_t* number,
                   FileType* type);

// Atomically points CURRENT at MANIFEST-<descriptor_number>: write a temp
// file, fsync it, rename it over CURRENT, fsync the directory. A crash at
// any point leaves either the old or the new CURRENT, never a torn one.
Status SetCurrentFile(Env* env, const std::string& dbname,
                      uint64_t descriptor_number);

}  // namespace lsmkv

#endif  // LSMKV_SRC_DB_FILENAME_H_
