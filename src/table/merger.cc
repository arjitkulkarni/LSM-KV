// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "table/merger.h"

#include <cassert>

#include "lsmkv/comparator.h"

namespace lsmkv {
namespace {

class MergingIterator final : public Iterator {
 public:
  MergingIterator(const Comparator* comparator,
                  std::vector<std::unique_ptr<Iterator>> children)
      : comparator_(comparator), children_(std::move(children)) {
    heap_.reserve(children_.size());
  }

  bool Valid() const override { return !heap_.empty(); }

  void SeekToFirst() override {
    for (auto& child : children_) child->SeekToFirst();
    RebuildHeap();
  }

  void Seek(const Slice& target) override {
    for (auto& child : children_) child->Seek(target);
    RebuildHeap();
  }

  void Next() override {
    assert(Valid());
    Iterator* top = children_[heap_[0]].get();
    top->Next();
    if (top->Valid()) {
      SiftDown(0);  // its key grew; restore the heap property
    } else {
      heap_[0] = heap_.back();  // child exhausted: pop it
      heap_.pop_back();
      if (!heap_.empty()) SiftDown(0);
    }
  }

  Slice key() const override {
    assert(Valid());
    return children_[heap_[0]]->key();
  }

  Slice value() const override {
    assert(Valid());
    return children_[heap_[0]]->value();
  }

  Status status() const override {
    for (const auto& child : children_) {
      Status s = child->status();
      if (!s.ok()) return s;
    }
    return Status::OK();
  }

 private:
  // Strict weak order over child indices: by current key, then by child
  // position (lower index = newer source wins ties).
  bool Less(size_t a, size_t b) const {
    const int r = comparator_->Compare(children_[a]->key(), children_[b]->key());
    if (r != 0) return r < 0;
    return a < b;
  }

  void RebuildHeap() {
    heap_.clear();
    for (size_t i = 0; i < children_.size(); i++) {
      if (children_[i]->Valid()) heap_.push_back(i);
    }
    // Floyd's O(k) heap construction.
    for (size_t i = heap_.size() / 2; i-- > 0;) SiftDown(i);
  }

  void SiftDown(size_t pos) {
    const size_t n = heap_.size();
    while (true) {
      const size_t left = 2 * pos + 1;
      if (left >= n) return;
      size_t smallest = left;
      const size_t right = left + 1;
      if (right < n && Less(heap_[right], heap_[left])) smallest = right;
      if (!Less(heap_[smallest], heap_[pos])) return;
      std::swap(heap_[smallest], heap_[pos]);
      pos = smallest;
    }
  }

  const Comparator* comparator_;
  std::vector<std::unique_ptr<Iterator>> children_;
  std::vector<size_t> heap_;  // indices into children_, min-heap
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(
    const Comparator* comparator,
    std::vector<std::unique_ptr<Iterator>> children) {
  if (children.empty()) return NewEmptyIterator();
  if (children.size() == 1) return std::move(children[0]);
  return std::make_unique<MergingIterator>(comparator, std::move(children));
}

}  // namespace lsmkv
