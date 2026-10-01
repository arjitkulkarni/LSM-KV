// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// lsmkv-dump: makes every on-disk format human-readable.
//
//   lsmkv-dump sst <file.sst> [--entries=N]
//   lsmkv-dump wal <file.log> [--entries=N]
//   lsmkv-dump manifest <MANIFEST-nnnnnn>
//   lsmkv-dump db <dbdir>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/version_edit.h"
#include "db/write_batch_internal.h"
#include "lsmkv/env.h"
#include "lsmkv/filter_policy.h"
#include "table/block.h"
#include "table/format.h"
#include "table/table.h"
#include "util/coding.h"
#include "wal/log_reader.h"

namespace lsmkv {
namespace {

std::string Escape(const Slice& s, size_t max_len = 40) {
  std::string out;
  for (size_t i = 0; i < s.size() && i < max_len; i++) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c >= ' ' && c <= '~' && c != '\'') {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    }
  }
  if (s.size() > max_len) out += "...";
  return out;
}

std::string HumanBytes(uint64_t n) {
  char buf[32];
  if (n >= (1ull << 20)) {
    std::snprintf(buf, sizeof(buf), "%.2f MiB", n / 1048576.0);
  } else if (n >= 1024) {
    std::snprintf(buf, sizeof(buf), "%.1f KiB", n / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
  }
  return buf;
}

std::string DescribeInternalKey(const Slice& ikey) {
  ParsedInternalKey p;
  if (!ParseInternalKey(ikey, &p)) return "(corrupt key) '" + Escape(ikey) + "'";
  return "'" + Escape(p.user_key) + "' @" + std::to_string(p.sequence) +
         (p.type == kTypeValue ? " PUT" : " DEL");
}

int DumpSSTable(const std::string& fname, int max_entries) {
  Env* env = Env::Default();
  uint64_t size = 0;
  Status s = env->GetFileSize(fname, &size);
  std::unique_ptr<RandomAccessFile> file;
  if (s.ok()) s = env->NewRandomAccessFile(fname, &file);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.ToString().c_str());
    return 1;
  }

  static const InternalKeyComparator icmp(BytewiseComparator());
  TableOptions topts;
  topts.comparator = &icmp;
  topts.filter_policy = std::make_shared<InternalFilterPolicy>(NewBloomFilterPolicy(10));
  std::shared_ptr<Table> table;
  s = Table::Open(topts, std::move(file), size, &table);
  if (!s.ok()) {
    std::fprintf(stderr, "cannot open table: %s\n", s.ToString().c_str());
    return 1;
  }

  const Footer& footer = table->footer();
  std::printf("SSTable %s  (%s)\n\n", fname.c_str(), HumanBytes(size).c_str());
  std::printf("FOOTER  (last %zu bytes)\n", Footer::kEncodedLength);
  std::printf("  magic           0x%016llx  \"lsmkvsst\"\n",
              static_cast<unsigned long long>(kTableMagicNumber));
  std::printf("  index block     offset=%-10llu size=%llu\n",
              static_cast<unsigned long long>(footer.index_handle().offset()),
              static_cast<unsigned long long>(footer.index_handle().size()));
  std::printf("  filter block    offset=%-10llu size=%llu  policy=%s\n\n",
              static_cast<unsigned long long>(footer.filter_handle().offset()),
              static_cast<unsigned long long>(footer.filter_handle().size()),
              table->filter_policy_name().empty() ? "(none)"
                                                  : table->filter_policy_name().c_str());

  // Walk the index block: one entry per data block.
  Block::Iter index(table->index_block().get(), &icmp);
  struct BlockInfo {
    uint64_t offset;
    uint64_t size;
    uint32_t restarts;
    uint64_t entries;
    std::string separator;
  };
  std::vector<BlockInfo> blocks;
  uint64_t entries = 0;
  uint64_t puts = 0;
  uint64_t dels = 0;
  uint64_t key_bytes = 0;
  uint64_t value_bytes = 0;
  uint64_t shared_bytes = 0;
  SequenceNumber min_seq = kMaxSequenceNumber;
  SequenceNumber max_seq = 0;
  std::vector<std::string> sample;

  for (index.SeekToFirst(); index.Valid(); index.Next()) {
    BlockHandle h;
    Slice v = index.value();
    if (!h.DecodeFrom(&v).ok()) {
      std::fprintf(stderr, "bad block handle in index\n");
      return 1;
    }
    std::shared_ptr<const Block> block;
    s = table->ReadBlockForDump(index.value(), &block);
    if (!s.ok()) {
      std::printf("  !! block @%llu: %s\n", static_cast<unsigned long long>(h.offset()),
                  s.ToString().c_str());
      continue;
    }
    BlockInfo info{h.offset(), h.size(), block->NumRestarts(), 0,
                   DescribeInternalKey(index.key())};
    Block::Iter it(block.get(), &icmp);
    for (it.SeekToFirst(); it.Valid(); it.Next()) {
      info.entries++;
      entries++;
      shared_bytes += it.shared_bytes();
      ParsedInternalKey p;
      if (ParseInternalKey(it.key(), &p)) {
        (p.type == kTypeValue ? puts : dels)++;
        key_bytes += p.user_key.size();
        min_seq = std::min(min_seq, p.sequence);
        max_seq = std::max(max_seq, p.sequence);
      }
      value_bytes += it.value().size();
      if (static_cast<int>(sample.size()) < max_entries) {
        sample.push_back(DescribeInternalKey(it.key()) + "  =>  '" +
                         Escape(it.value(), 32) + "' (" +
                         std::to_string(it.value().size()) + " B)");
      }
    }
    blocks.push_back(info);
  }

  std::printf("INDEX  (%zu data blocks, binary-searched on every lookup)\n",
              blocks.size());
  std::printf("  %-5s %-10s %-6s %-8s %-7s %s\n", "#", "offset", "size", "restarts",
              "entries", "separator key (>= every key in the block)");
  const size_t show = blocks.size() <= 12 ? blocks.size() : 10;
  for (size_t i = 0; i < show; i++) {
    const BlockInfo& b = blocks[i];
    std::printf("  %-5zu %-10llu %-6llu %-8u %-7llu %s\n", i,
                static_cast<unsigned long long>(b.offset),
                static_cast<unsigned long long>(b.size), b.restarts,
                static_cast<unsigned long long>(b.entries), b.separator.c_str());
  }
  if (show < blocks.size()) {
    std::printf("  ... %zu more blocks ...\n", blocks.size() - show);
    const BlockInfo& b = blocks.back();
    std::printf("  %-5zu %-10llu %-6llu %-8u %-7llu %s\n", blocks.size() - 1,
                static_cast<unsigned long long>(b.offset),
                static_cast<unsigned long long>(b.size), b.restarts,
                static_cast<unsigned long long>(b.entries), b.separator.c_str());
  }

  const Slice filter = table->filter_data();
  std::printf("\nBLOOM FILTER\n");
  if (filter.size() >= 2) {
    const uint64_t bits = (filter.size() - 1) * 8;
    const int k = static_cast<uint8_t>(filter[filter.size() - 1]);
    std::printf("  %llu bits (%s), k=%d probes, %.1f bits/key\n",
                static_cast<unsigned long long>(bits), HumanBytes(filter.size()).c_str(),
                k, entries ? static_cast<double>(bits) / entries : 0.0);
  } else {
    std::printf("  (none)\n");
  }

  std::printf("\nSUMMARY\n");
  std::printf("  entries          %llu  (%llu puts, %llu tombstones)\n",
              static_cast<unsigned long long>(entries),
              static_cast<unsigned long long>(puts), static_cast<unsigned long long>(dels));
  if (entries > 0) {
    std::printf("  sequence range   [%llu, %llu]\n",
                static_cast<unsigned long long>(min_seq),
                static_cast<unsigned long long>(max_seq));
    std::printf("  user keys        %s (avg %.1f B)\n", HumanBytes(key_bytes).c_str(),
                static_cast<double>(key_bytes) / entries);
    std::printf("  values           %s (avg %.1f B)\n", HumanBytes(value_bytes).c_str(),
                static_cast<double>(value_bytes) / entries);
    const uint64_t ikey_bytes = key_bytes + 8 * entries;
    std::printf("  prefix compression saved %s of %s internal-key bytes (%.1f%%)\n",
                HumanBytes(shared_bytes).c_str(), HumanBytes(ikey_bytes).c_str(),
                100.0 * static_cast<double>(shared_bytes) / static_cast<double>(ikey_bytes));
  }

  if (!sample.empty()) {
    std::printf("\nENTRIES  (first %zu)\n", sample.size());
    for (const auto& line : sample) std::printf("  %s\n", line.c_str());
  }
  return 0;
}

class PrintingReporter final : public log::Reader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& s) override {
    std::printf("  !! corruption: %zu bytes dropped: %s\n", bytes, s.ToString().c_str());
    count++;
  }
  int count = 0;
};

class BatchPrinter final : public WriteBatch::Handler {
 public:
  explicit BatchPrinter(int max) : max_(max) {}
  void Put(const Slice& key, const Slice& value) override {
    if (shown_++ < max_) {
      std::printf("      PUT '%s' => %zu B\n", Escape(key).c_str(), value.size());
    }
  }
  void Delete(const Slice& key) override {
    if (shown_++ < max_) std::printf("      DEL '%s'\n", Escape(key).c_str());
  }

 private:
  int max_;
  int shown_ = 0;
};

int DumpWal(const std::string& fname, int max_entries) {
  Env* env = Env::Default();
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.ToString().c_str());
    return 1;
  }
  uint64_t size = 0;
  (void)env->GetFileSize(fname, &size);
  const auto blocks = static_cast<unsigned long long>((size + 32767) / 32768);
  std::printf("WAL %s  (%s, %llu block%s of 32 KiB)\n\n", fname.c_str(),
              HumanBytes(size).c_str(), blocks, blocks == 1 ? "" : "s");
  PrintingReporter reporter;
  log::Reader reader(file.get(), &reporter, true);
  std::string scratch;
  Slice record;
  uint64_t records = 0;
  uint64_t ops = 0;
  SequenceNumber first = 0;
  SequenceNumber last = 0;
  std::printf("  %-10s %-8s %-12s %-6s\n", "offset", "bytes", "first seq", "ops");
  while (reader.ReadRecord(&record, &scratch)) {
    records++;
    if (record.size() < WriteBatchInternal::kHeader) {
      std::printf("  !! short record (%zu bytes)\n", record.size());
      continue;
    }
    WriteBatch batch;
    WriteBatchInternal::SetContents(&batch, record);
    const SequenceNumber seq = WriteBatchInternal::Sequence(&batch);
    const uint32_t count = WriteBatchInternal::Count(&batch);
    if (records == 1) first = seq;
    last = seq + count - 1;
    ops += count;
    if (static_cast<int>(records) <= max_entries) {
      std::printf("  %-10llu %-8zu %-12llu %-6u%s\n",
                  static_cast<unsigned long long>(reader.LastRecordOffset()), record.size(),
                  static_cast<unsigned long long>(seq), count,
                  count > 1 ? "  (group commit / batch)" : "");
      BatchPrinter printer(3);
      (void)batch.Iterate(&printer);
    }
  }
  std::printf("\nSUMMARY\n  records %llu, operations %llu, sequence [%llu, %llu]\n",
              static_cast<unsigned long long>(records), static_cast<unsigned long long>(ops),
              static_cast<unsigned long long>(first), static_cast<unsigned long long>(last));
  std::printf("  mean operations per record: %.2f\n",
              records ? static_cast<double>(ops) / records : 0.0);
  std::printf("  valid prefix ends at byte %llu of %llu%s\n",
              static_cast<unsigned long long>(reader.LastRecordEndOffset()),
              static_cast<unsigned long long>(size),
              reader.truncated_tail() ? "  (torn final record)" : "");
  std::printf("  corruption events: %d\n", reporter.count);
  return reporter.count == 0 ? 0 : 2;
}

int DumpManifest(const std::string& fname, bool quiet,
                 std::map<int, std::map<uint64_t, FileMetaData>>* live) {
  Env* env = Env::Default();
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.ToString().c_str());
    return 1;
  }
  PrintingReporter reporter;
  log::Reader reader(file.get(), &reporter, true);
  std::string scratch;
  Slice record;
  int n = 0;
  while (reader.ReadRecord(&record, &scratch)) {
    VersionEdit edit;
    s = edit.DecodeFrom(record);
    if (!s.ok()) {
      std::printf("  !! %s\n", s.ToString().c_str());
      continue;
    }
    if (!quiet) std::printf("#%d %s", n, edit.DebugString().c_str());
    n++;
    if (live != nullptr) {
      for (const auto& [level, number] : edit.deleted_files()) {
        (*live)[level].erase(number);
      }
      for (const auto& [level, f] : edit.new_files()) (*live)[level][f.number] = f;
    }
  }
  if (!quiet) std::printf("%d edits\n", n);
  return 0;
}

int DumpDB(const std::string& dir) {
  Env* env = Env::Default();
  std::string current;
  Status s = ReadFileToString(env, CurrentFileName(dir), &current);
  if (!s.ok()) {
    std::fprintf(stderr, "no CURRENT: %s\n", s.ToString().c_str());
    return 1;
  }
  while (!current.empty() && (current.back() == '\n' || current.back() == '\r')) {
    current.pop_back();
  }
  std::printf("DATABASE %s\n  CURRENT -> %s\n\n", dir.c_str(), current.c_str());
  std::map<int, std::map<uint64_t, FileMetaData>> live;
  if (DumpManifest(dir + "/" + current, /*quiet=*/true, &live) != 0) return 1;

  std::printf("LEVELS\n");
  uint64_t total = 0;
  for (int level = 0; level < config::kNumLevels; level++) {
    const auto& files = live[level];
    uint64_t bytes = 0;
    for (const auto& [num, f] : files) bytes += f.file_size;
    total += bytes;
    if (files.empty()) continue;
    std::printf("  L%d  %3zu files  %10s\n", level, files.size(), HumanBytes(bytes).c_str());
    int shown = 0;
    for (const auto& [num, f] : files) {
      if (shown++ >= 4) {
        std::printf("        ...\n");
        break;
      }
      std::printf("        %06llu.sst %9s  [%s .. %s]\n",
                  static_cast<unsigned long long>(num), HumanBytes(f.file_size).c_str(),
                  Escape(f.smallest.user_key(), 20).c_str(),
                  Escape(f.largest.user_key(), 20).c_str());
    }
  }
  std::printf("  total SSTable bytes: %s\n\n", HumanBytes(total).c_str());

  std::vector<std::string> children;
  (void)env->GetChildren(dir, &children);
  std::printf("WRITE-AHEAD LOGS\n");
  for (const auto& name : children) {
    uint64_t number;
    FileType type;
    if (ParseFileName(name, &number, &type) && type == FileType::kLogFile) {
      uint64_t size = 0;
      (void)env->GetFileSize(dir + "/" + name, &size);
      std::printf("  %s  %s\n", name.c_str(), HumanBytes(size).c_str());
    }
  }
  return 0;
}

int Usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  lsmkv-dump sst <file.sst> [--entries=N]\n"
               "  lsmkv-dump wal <file.log> [--entries=N]\n"
               "  lsmkv-dump manifest <MANIFEST-nnnnnn>\n"
               "  lsmkv-dump db <dbdir>\n");
  return 64;
}

}  // namespace
}  // namespace lsmkv

int main(int argc, char** argv) {
  using namespace lsmkv;
  if (argc < 3) return Usage();
  int entries = 10;
  for (int i = 3; i < argc; i++) {
    if (std::strncmp(argv[i], "--entries=", 10) == 0) entries = std::atoi(argv[i] + 10);
  }
  const std::string cmd = argv[1];
  const std::string target = argv[2];
  if (cmd == "sst") return DumpSSTable(target, entries);
  if (cmd == "wal") return DumpWal(target, entries);
  if (cmd == "manifest") return DumpManifest(target, false, nullptr);
  if (cmd == "db") return DumpDB(target);
  return Usage();
}
