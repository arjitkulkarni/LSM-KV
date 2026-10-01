// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "table/table.h"

#include "lsmkv/cache.h"
#include "lsmkv/comparator.h"
#include "lsmkv/metrics.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"

namespace lsmkv {

namespace {
inline void Bump(Counter* c, uint64_t n = 1) {
  if (c != nullptr) c->Inc(n);
}
}  // namespace

Table::~Table() = default;

Status Table::Open(const TableOptions& options,
                   std::unique_ptr<RandomAccessFile> file, uint64_t size,
                   std::shared_ptr<Table>* table) {
  table->reset();
  if (size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_input, footer_space);
  if (!s.ok()) return s;

  Footer footer;
  s = footer.DecodeFrom(&footer_input);
  if (!s.ok()) return s;

  std::string index_contents;
  s = ReadBlock(file.get(), footer.index_handle(), &index_contents);
  if (!s.ok()) return s;

  std::shared_ptr<Table> t(new Table(options));
  t->file_ = std::move(file);
  t->file_size_ = size;
  t->footer_ = footer;
  t->index_block_ = std::make_shared<const Block>(std::move(index_contents));
  if (t->index_block_->malformed()) {
    return Status::Corruption("malformed index block");
  }
  if (options.block_cache != nullptr) {
    t->cache_id_ = options.block_cache->NewId();
  }

  // The filter is an optimization: if it is missing, damaged or from a
  // different policy, the table is still fully readable without it.
  if (footer.filter_handle().size() > 0 && options.filter_policy != nullptr) {
    std::string filter_contents;
    if (ReadBlock(t->file_.get(), footer.filter_handle(), &filter_contents)
            .ok()) {
      Slice input(filter_contents);
      Slice name;
      if (GetLengthPrefixedSlice(&input, &name)) {
        t->filter_name_ = name.ToString();
        if (t->filter_name_ == options.filter_policy->Name()) {
          t->filter_data_ = input.ToString();
        }
      }
    }
  }

  *table = std::move(t);
  return Status::OK();
}

bool Table::KeyMayMatch(const Slice& key) const {
  if (filter_data_.empty() || options_.filter_policy == nullptr) return true;
  return options_.filter_policy->KeyMayMatch(key, Slice(filter_data_));
}

std::shared_ptr<const Block> Table::ReadDataBlock(const ReadOptions& options,
                                                  const Slice& index_value,
                                                  Status* s) const {
  BlockHandle handle;
  Slice input = index_value;
  *s = handle.DecodeFrom(&input);
  if (!s->ok()) return nullptr;

  Cache* cache = options_.block_cache.get();
  char cache_key_buf[16];
  if (cache != nullptr) {
    // Key = (per-table id, block offset): unique across every table that
    // shares this cache, and stable for the table's lifetime.
    EncodeFixed64(cache_key_buf, cache_id_);
    EncodeFixed64(cache_key_buf + 8, handle.offset());
    const Slice key(cache_key_buf, sizeof(cache_key_buf));
    std::shared_ptr<void> cached = cache->Lookup(key);
    if (cached != nullptr) {
      Bump(options_.counters.cache_hits);
      return std::static_pointer_cast<const Block>(cached);
    }
    Bump(options_.counters.cache_misses);
  }

  std::string contents;
  *s = ReadBlock(file_.get(), handle, &contents);
  if (!s->ok()) return nullptr;
  Bump(options_.counters.block_reads);
  Bump(options_.counters.block_read_bytes, handle.size() + kBlockTrailerSize);

  auto block = std::make_shared<Block>(std::move(contents));
  if (block->malformed()) {
    *s = Status::Corruption("malformed data block");
    return nullptr;
  }
  if (cache != nullptr && options.fill_cache) {
    cache->Insert(Slice(cache_key_buf, sizeof(cache_key_buf)),
                  std::shared_ptr<void>(block), block->size());
  }
  return block;
}

Status Table::ReadBlockForDump(const Slice& index_value,
                               std::shared_ptr<const Block>* block) const {
  Status s;
  ReadOptions opts;
  opts.fill_cache = false;
  *block = ReadDataBlock(opts, index_value, &s);
  return s;
}

std::unique_ptr<Iterator> Table::NewIterator(const ReadOptions& options) const {
  std::shared_ptr<const Table> self = shared_from_this();
  auto block_function =
      [self, options](const Slice& index_value) -> std::unique_ptr<Iterator> {
    Status s;
    std::shared_ptr<const Block> block =
        self->ReadDataBlock(options, index_value, &s);
    if (!s.ok()) return NewErrorIterator(s);
    return NewBlockIterator(std::move(block), self->options_.comparator);
  };
  return NewTwoLevelIterator(
      NewBlockIterator(index_block_, options_.comparator),
      std::move(block_function));
}

Status Table::InternalGet(
    const ReadOptions& options, const Slice& key,
    const std::function<void(const Slice&, const Slice&)>& handle_result)
    const {
  // 1. Bloom filter: most lookups for absent keys stop here, without I/O.
  if (!filter_data_.empty() && options_.filter_policy != nullptr) {
    Bump(options_.counters.bloom_checks);
    if (!options_.filter_policy->KeyMayMatch(key, Slice(filter_data_))) {
      Bump(options_.counters.bloom_negatives);
      return Status::OK();
    }
  }

  // 2. Binary search the pinned index for the one block that may hold key.
  //    (Stack-allocated cursors: a point lookup does no heap allocation for
  //    iteration.)
  Block::Iter index_iter(index_block_.get(), options_.comparator);
  index_iter.Seek(key);
  if (!index_iter.Valid()) return index_iter.status();

  // 3. Fetch that block (cache, else one positional read + CRC check) and
  //    binary search it.
  Status s;
  std::shared_ptr<const Block> block =
      ReadDataBlock(options, index_iter.value(), &s);
  if (!s.ok()) return s;
  Block::Iter block_iter(block.get(), options_.comparator);
  block_iter.Seek(key);
  if (block_iter.Valid()) handle_result(block_iter.key(), block_iter.value());
  return block_iter.status();
}

}  // namespace lsmkv
