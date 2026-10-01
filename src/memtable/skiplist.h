// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// A probabilistic skiplist (Pugh, 1990) that supports one writer and any
// number of concurrent, lock-free readers.

#ifndef LSMKV_SRC_MEMTABLE_SKIPLIST_H_
#define LSMKV_SRC_MEMTABLE_SKIPLIST_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>

#include "util/arena.h"
#include "util/random.h"

namespace lsmkv {

// Why a skiplist and not a red-black tree (see README for the long form):
//  * Concurrency: an insert changes at most kMaxHeight forward pointers,
//    each published with a single release-store *after* the new node is
//    fully built. A reader that follows pointers with acquire-loads always
//    sees a consistent list, so reads need no lock at all. A red-black tree
//    rebalances (rotations touch several nodes at once) and cannot offer
//    lock-free reads this cheaply.
//  * Nodes never move or get freed individually, which is exactly what the
//    arena allocator wants.
//  * Ordered iteration is a walk along level 0.
//
// Expected cost: with promotion probability p = 1/4 a node has height h
// with probability (1-p)p^(h-1); expected height 1/(1-p) = 1.33 pointers
// per node. Search visits O(log_{1/p} n) levels and an expected 1/p nodes
// per level, i.e. O(log n) comparisons. kMaxHeight = 12 covers
// n ~ 4^12 = 16.7M entries per list before the top level saturates.
//
// Thread safety:
//   Writes (Insert) require external synchronization (a mutex).
//   Reads require only that the SkipList is not destroyed while reading.
template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  static constexpr int kMaxHeight = 12;
  static constexpr unsigned kBranching = 4;  // p = 1/4

  // Uses `arena` for all node memory. `cmp` orders keys.
  SkipList(Comparator cmp, Arena* arena, uint64_t seed = 0xdeadbeefull);
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // REQUIRES: nothing that compares equal to key is in the list, and the
  // caller holds the writer lock.
  void Insert(const Key& key);

  bool Contains(const Key& key) const;

  // Forward iteration. Safe to use while another thread inserts.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

    bool Valid() const { return node_ != nullptr; }
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    // Positions at the first entry with key >= target.
    void Seek(const Key& target) {
      node_ = list_->FindGreaterOrEqual(target, nullptr);
    }
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  int GetMaxHeight() const {
    return max_height_.load(std::memory_order_relaxed);
  }

  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }

  // True if key is strictly greater than the key stored in n.
  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return (n != nullptr) && (compare_(n->key, key) < 0);
  }

  // Returns the earliest node >= key, filling prev[level] with the last
  // node < key at every level when prev != nullptr.
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;
  // Height of the tallest node. Readers may see a stale (smaller) value,
  // which only makes their search start lower -- still correct.
  std::atomic<int> max_height_;
  Random rnd_;  // writer-only
};

template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}

  Key const key;

  // Acquire-load: everything the inserting thread wrote to the node before
  // publishing it (key bytes, lower-level links) is visible to us.
  Node* Next(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_acquire);
  }
  // Release-store: publishes this node's pointer only after its contents.
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_release);
  }
  // Relaxed variants, safe only while the node is not yet reachable.
  Node* NoBarrierNext(int n) { return next_[n].load(std::memory_order_relaxed); }
  void NoBarrierSetNext(int n, Node* x) {
    next_[n].store(x, std::memory_order_relaxed);
  }

  // Variable-length: a node of height h is allocated with room for h links.
  // Must remain the last member.
  std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
  char* const mem = arena_->AllocateAligned(
      sizeof(Node) + sizeof(std::atomic<Node*>) * static_cast<size_t>(height - 1));
  Node* node = new (mem) Node(key);
  // The links beyond next_[0] live in memory past the declared array; give
  // each one a properly constructed atomic.
  for (int i = 1; i < height; i++) {
    new (&node->next_[i]) std::atomic<Node*>(nullptr);
  }
  node->next_[0].store(nullptr, std::memory_order_relaxed);
  return node;
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  int height = 1;
  while (height < kMaxHeight && rnd_.OneIn(kBranching)) {
    height++;
  }
  assert(height > 0 && height <= kMaxHeight);
  return height;
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key,
                                              Node** prev) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      x = next;  // keep moving right on this level
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) return next;
      level--;  // drop down a level
    }
  }
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena,
                                    uint64_t seed)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(Key() /* any key will do */, kMaxHeight)),
      max_height_(1),
      rnd_(seed) {
  for (int i = 0; i < kMaxHeight; i++) {
    head_->SetNext(i, nullptr);
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);

  // Duplicate insertion is not allowed (internal keys carry unique
  // sequence numbers, so this never happens in the engine).
  assert(x == nullptr || !Equal(key, x->key));

  const int height = RandomHeight();
  if (height > GetMaxHeight()) {
    for (int i = GetMaxHeight(); i < height; i++) {
      prev[i] = head_;
    }
    // A concurrent reader that observes the new height before the new
    // node's links will see nullptr from head_ at those levels and simply
    // drop down, so a relaxed store is sufficient.
    max_height_.store(height, std::memory_order_relaxed);
  }

  x = NewNode(key, height);
  for (int i = 0; i < height; i++) {
    // Link bottom-up. x is unreachable until prev[i]->SetNext, so the
    // relaxed store into x is fine; SetNext's release makes it visible.
    x->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
    prev[i]->SetNext(i, x);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && Equal(key, x->key);
}

}  // namespace lsmkv

#endif  // LSMKV_SRC_MEMTABLE_SKIPLIST_H_
