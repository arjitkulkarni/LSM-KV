// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "db/builder.h"

#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "lsmkv/env.h"
#include "lsmkv/iterator.h"
#include "table/table_builder.h"

namespace lsmkv {

Status BuildTable(const std::string& dbname, Env* env,
                  const TableOptions& options,
                  const InternalKeyComparator& icmp, TableCache* table_cache,
                  Iterator* iter, FileMetaData* meta, BuildTableStats* stats) {
  Status s;
  meta->file_size = 0;
  iter->SeekToFirst();

  const std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    std::unique_ptr<WritableFile> file;
    s = env->NewWritableFile(fname, &file);
    if (!s.ok()) return s;

    TableBuilder builder(options, file.get());
    const Comparator* ucmp = icmp.user_comparator();
    std::string current_user_key;
    bool has_current = false;
    for (; iter->Valid(); iter->Next()) {
      const Slice key = iter->key();
      if (stats != nullptr) stats->entries_in++;
      const Slice user_key = ExtractUserKey(key);
      if (has_current && ucmp->Compare(user_key, Slice(current_user_key)) == 0) {
        continue;  // an older version, shadowed by the one just written
      }
      current_user_key.assign(user_key.data(), user_key.size());
      has_current = true;
      if (builder.NumEntries() == 0) meta->smallest.DecodeFrom(key);
      meta->largest.DecodeFrom(key);
      builder.Add(key, iter->value());
      if (stats != nullptr) stats->entries_out++;
    }

    s = builder.Finish();
    if (s.ok()) {
      meta->file_size = builder.FileSize();
    }

    // Durable before it can be referenced by the MANIFEST.
    if (s.ok()) {
      const uint64_t t0 = env->NowNanos();
      s = file->Sync();
      if (stats != nullptr) stats->sync_nanos = env->NowNanos() - t0;
    }
    if (s.ok()) s = file->Close();
    file.reset();

    if (s.ok()) {
      // Verify that the table is usable.
      std::shared_ptr<Table> table;
      s = table_cache->FindTable(meta->number, meta->file_size, &table);
    }
  }

  // Check for input iterator errors.
  if (s.ok() && !iter->status().ok()) s = iter->status();

  if (!s.ok() || meta->file_size == 0) {
    (void)env->RemoveFile(fname);
    if (s.ok()) meta->file_size = 0;
  }
  return s;
}

}  // namespace lsmkv
