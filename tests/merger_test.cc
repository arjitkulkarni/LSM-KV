// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "table/merger.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

#include "lsmkv/comparator.h"
#include "test_util.h"
#include "util/random.h"

namespace lsmkv {

namespace {

// A sorted vector exposed as an Iterator.
class VectorIterator final : public Iterator {
 public:
  explicit VectorIterator(std::vector<std::string> keys, std::string tag)
      : keys_(std::move(keys)), tag_(std::move(tag)), pos_(keys_.size()) {}
  bool Valid() const override { return pos_ < keys_.size(); }
  void SeekToFirst() override { pos_ = 0; }
  void Seek(const Slice& target) override {
    pos_ = static_cast<size_t>(
        std::lower_bound(keys_.begin(), keys_.end(), target.ToString()) -
        keys_.begin());
  }
  void Next() override { pos_++; }
  Slice key() const override { return keys_[pos_]; }
  Slice value() const override { return tag_; }
  Status status() const override { return Status::OK(); }

 private:
  std::vector<std::string> keys_;
  std::string tag_;
  size_t pos_;
};

}  // namespace

TEST(MergerTest, MergesManySortedRunsInOrder) {
  Random rnd(7);
  std::multiset<std::string> all;
  std::vector<std::unique_ptr<Iterator>> children;
  for (int c = 0; c < 17; c++) {  // an odd number exercises heap edges
    std::vector<std::string> keys;
    const int n = static_cast<int>(rnd.Uniform(300));
    for (int i = 0; i < n; i++) keys.push_back(test::Key(rnd.Uniform(100000)));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    all.insert(keys.begin(), keys.end());
    children.push_back(std::make_unique<VectorIterator>(keys, std::to_string(c)));
  }
  auto merged = NewMergingIterator(BytewiseComparator(), std::move(children));
  merged->SeekToFirst();
  for (const std::string& k : all) {
    ASSERT_TRUE(merged->Valid());
    ASSERT_EQ(k, merged->key().ToString());
    merged->Next();
  }
  EXPECT_FALSE(merged->Valid());

  // Seek then scan agrees with the model.
  const std::string target = test::Key(50000);
  merged->Seek(target);
  auto it = all.lower_bound(target);
  for (int i = 0; i < 100 && it != all.end(); i++, ++it) {
    ASSERT_TRUE(merged->Valid());
    ASSERT_EQ(*it, merged->key().ToString());
    merged->Next();
  }
}

TEST(MergerTest, TiesGoToTheFirstChild) {
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(std::make_unique<VectorIterator>(
      std::vector<std::string>{"a", "b"}, "newest"));
  children.push_back(std::make_unique<VectorIterator>(
      std::vector<std::string>{"b", "c"}, "oldest"));
  auto merged = NewMergingIterator(BytewiseComparator(), std::move(children));
  merged->SeekToFirst();
  ASSERT_EQ("a", merged->key().ToString());
  merged->Next();
  ASSERT_EQ("b", merged->key().ToString());
  EXPECT_EQ("newest", merged->value().ToString());
  merged->Next();
  ASSERT_EQ("b", merged->key().ToString());
  EXPECT_EQ("oldest", merged->value().ToString());
}

TEST(MergerTest, EmptyChildren) {
  std::vector<std::unique_ptr<Iterator>> none;
  auto merged = NewMergingIterator(BytewiseComparator(), std::move(none));
  merged->SeekToFirst();
  EXPECT_FALSE(merged->Valid());
}

}  // namespace lsmkv
