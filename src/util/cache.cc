// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Sharded LRU cache.
//
// Each shard is the textbook O(1) LRU: an unordered_map from key to node,
// plus an intrusive circular doubly-linked list ordered by recency. The list
// links live inside the node itself (no separate std::list allocation), and
// the map's key is a string_view into the node's own key bytes, so each
// entry costs exactly one heap allocation.

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lsmkv/cache.h"
#include "util/hash.h"

namespace lsmkv {

Cache::~Cache() = default;

namespace {

struct LRUNode {
  std::string key;
  std::shared_ptr<void> value;
  size_t charge = 0;
  LRUNode* prev = nullptr;
  LRUNode* next = nullptr;
};

class LRUShard {
 public:
  LRUShard() {
    head_.next = &head_;
    head_.prev = &head_;
  }
  LRUShard(const LRUShard&) = delete;
  LRUShard& operator=(const LRUShard&) = delete;

  ~LRUShard() {
    for (auto& kv : index_) {
      delete kv.second;  // nodes are owned by the shard
    }
  }

  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  void Insert(const Slice& key, std::shared_ptr<void> value, size_t charge,
              std::vector<std::shared_ptr<void>>* evicted) {
    std::lock_guard<std::mutex> l(mu_);
    inserts_++;
    if (capacity_ == 0) return;  // caching disabled
    auto it = index_.find(key.ToStringView());
    if (it != index_.end()) {
      LRUNode* old = it->second;
      Unlink(old);
      usage_ -= old->charge;
      evicted->push_back(std::move(old->value));
      index_.erase(it);
      delete old;
    }
    auto* node = new LRUNode;
    node->key.assign(key.data(), key.size());
    node->value = std::move(value);
    node->charge = charge;
    LinkAtFront(node);
    usage_ += charge;
    index_.emplace(std::string_view(node->key), node);

    // Evict from the cold end until we fit. The entry just inserted is never
    // evicted by its own insertion unless it alone exceeds the capacity.
    while (usage_ > capacity_ && head_.prev != &head_) {
      LRUNode* victim = head_.prev;
      Unlink(victim);
      usage_ -= victim->charge;
      index_.erase(std::string_view(victim->key));
      evicted->push_back(std::move(victim->value));
      delete victim;
      evictions_++;
    }
  }

  std::shared_ptr<void> Lookup(const Slice& key) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = index_.find(key.ToStringView());
    if (it == index_.end()) {
      misses_++;
      return nullptr;
    }
    hits_++;
    LRUNode* node = it->second;
    Unlink(node);
    LinkAtFront(node);
    return node->value;
  }

  std::shared_ptr<void> Erase(const Slice& key) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = index_.find(key.ToStringView());
    if (it == index_.end()) return nullptr;
    LRUNode* node = it->second;
    Unlink(node);
    usage_ -= node->charge;
    index_.erase(it);
    std::shared_ptr<void> v = std::move(node->value);
    delete node;
    return v;
  }

  void AddStats(CacheStats* s) const {
    std::lock_guard<std::mutex> l(mu_);
    s->hits += hits_;
    s->misses += misses_;
    s->inserts += inserts_;
    s->evictions += evictions_;
    s->usage += usage_;
  }

  size_t Usage() const {
    std::lock_guard<std::mutex> l(mu_);
    return usage_;
  }

 private:
  void Unlink(LRUNode* n) {
    n->next->prev = n->prev;
    n->prev->next = n->next;
  }
  void LinkAtFront(LRUNode* n) {  // front == most recently used
    n->next = head_.next;
    n->prev = &head_;
    n->next->prev = n;
    head_.next = n;
  }

  mutable std::mutex mu_;
  size_t capacity_ = 0;
  size_t usage_ = 0;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
  uint64_t inserts_ = 0;
  uint64_t evictions_ = 0;
  LRUNode head_;  // sentinel of the circular list
  std::unordered_map<std::string_view, LRUNode*> index_;
};

class ShardedLRUCache final : public Cache {
 public:
  ShardedLRUCache(size_t capacity, int num_shard_bits)
      : shard_bits_(std::clamp(num_shard_bits, 0, 8)),
        shards_(size_t{1} << shard_bits_),
        capacity_(capacity) {
    const size_t n = shards_.size();
    const size_t per_shard = (capacity + n - 1) / n;
    for (auto& s : shards_) s.SetCapacity(per_shard);
  }

  void Insert(const Slice& key, std::shared_ptr<void> value,
              size_t charge) override {
    // Destroy evicted values after the shard lock is released: freeing a
    // 4 KB block (or closing a table file) under the lock would lengthen
    // the critical section for every other reader of the shard.
    std::vector<std::shared_ptr<void>> evicted;
    Shard(key).Insert(key, std::move(value), charge, &evicted);
  }

  std::shared_ptr<void> Lookup(const Slice& key) override {
    return Shard(key).Lookup(key);
  }

  void Erase(const Slice& key) override {
    std::shared_ptr<void> v = Shard(key).Erase(key);
  }

  uint64_t NewId() override {
    return next_id_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  size_t TotalCharge() const override {
    size_t total = 0;
    for (const auto& s : shards_) total += s.Usage();
    return total;
  }

  size_t Capacity() const override { return capacity_; }

  CacheStats GetStats() const override {
    CacheStats stats;
    for (const auto& s : shards_) s.AddStats(&stats);
    stats.capacity = capacity_;
    return stats;
  }

 private:
  LRUShard& Shard(const Slice& key) {
    if (shard_bits_ == 0) return shards_[0];
    return shards_[Hash64(key) >> (64 - shard_bits_)];
  }

  const int shard_bits_;
  std::vector<LRUShard> shards_;
  const size_t capacity_;
  std::atomic<uint64_t> next_id_{0};
};

}  // namespace

std::shared_ptr<Cache> NewLRUCache(size_t capacity, int num_shard_bits) {
  return std::make_shared<ShardedLRUCache>(capacity, num_shard_bits);
}

}  // namespace lsmkv
