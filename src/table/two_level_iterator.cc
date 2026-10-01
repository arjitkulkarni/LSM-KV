// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "table/two_level_iterator.h"

#include <string>

namespace lsmkv {
namespace {

class TwoLevelIterator final : public Iterator {
 public:
  TwoLevelIterator(std::unique_ptr<Iterator> index_iter,
                   BlockFunction block_function)
      : block_function_(std::move(block_function)),
        index_iter_(std::move(index_iter)) {}

  void Seek(const Slice& target) override {
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_ != nullptr) data_iter_->Seek(target);
    SkipEmptyDataBlocksForward();
  }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_ != nullptr) data_iter_->SeekToFirst();
    SkipEmptyDataBlocksForward();
  }

  void Next() override {
    data_iter_->Next();
    SkipEmptyDataBlocksForward();
  }

  bool Valid() const override {
    return data_iter_ != nullptr && data_iter_->Valid();
  }
  Slice key() const override { return data_iter_->key(); }
  Slice value() const override { return data_iter_->value(); }

  Status status() const override {
    Status s = index_iter_->status();
    if (!s.ok()) return s;
    if (data_iter_ != nullptr) {
      s = data_iter_->status();
      if (!s.ok()) return s;
    }
    return status_;
  }

 private:
  void SaveError(const Status& s) {
    if (status_.ok() && !s.ok()) status_ = s;
  }

  void SkipEmptyDataBlocksForward() {
    while (data_iter_ == nullptr || !data_iter_->Valid()) {
      // Move to the next block.
      if (!index_iter_->Valid()) {
        SetDataIterator(nullptr);
        return;
      }
      index_iter_->Next();
      InitDataBlock();
      if (data_iter_ != nullptr) data_iter_->SeekToFirst();
    }
  }

  void SetDataIterator(std::unique_ptr<Iterator> data_iter) {
    // Keep the first error we saw, even after moving past the block.
    if (data_iter_ != nullptr) SaveError(data_iter_->status());
    data_iter_ = std::move(data_iter);
  }

  void InitDataBlock() {
    if (!index_iter_->Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    const Slice handle = index_iter_->value();
    if (data_iter_ != nullptr && handle == Slice(data_block_handle_)) {
      // data_iter_ is already constructed with this block, so no need to
      // change anything.
      return;
    }
    SetDataIterator(block_function_(handle));
    data_block_handle_.assign(handle.data(), handle.size());
  }

  BlockFunction block_function_;
  Status status_;
  std::unique_ptr<Iterator> index_iter_;
  std::unique_ptr<Iterator> data_iter_;  // may be nullptr
  // If data_iter_ is non-null, the index value that produced it.
  std::string data_block_handle_;
};

}  // namespace

std::unique_ptr<Iterator> NewTwoLevelIterator(
    std::unique_ptr<Iterator> index_iter, BlockFunction block_function) {
  return std::make_unique<TwoLevelIterator>(std::move(index_iter),
                                            std::move(block_function));
}

}  // namespace lsmkv
