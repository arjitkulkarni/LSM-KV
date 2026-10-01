// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "lsmkv/iterator.h"

namespace lsmkv {

Iterator::~Iterator() = default;

namespace {

class EmptyIterator final : public Iterator {
 public:
  explicit EmptyIterator(Status s) : status_(std::move(s)) {}
  bool Valid() const override { return false; }
  void Seek(const Slice&) override {}
  void SeekToFirst() override {}
  void Next() override {}
  Slice key() const override { return Slice(); }
  Slice value() const override { return Slice(); }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewEmptyIterator() {
  return std::make_unique<EmptyIterator>(Status::OK());
}

std::unique_ptr<Iterator> NewErrorIterator(const Status& status) {
  return std::make_unique<EmptyIterator>(status);
}

}  // namespace lsmkv
