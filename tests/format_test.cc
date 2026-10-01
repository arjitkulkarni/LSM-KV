// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Block-level format: prefix compression, restart points, binary search.

#include <gtest/gtest.h>

#include <map>

#include "lsmkv/comparator.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "table/format.h"
#include "test_util.h"
#include "util/random.h"

namespace lsmkv {

TEST(FormatTest, BlockHandleRoundTrip) {
  BlockHandle h;
  h.set_offset(123456789);
  h.set_size(4096);
  std::string enc;
  h.EncodeTo(&enc);
  BlockHandle d;
  Slice in(enc);
  ASSERT_TRUE(d.DecodeFrom(&in).ok());
  EXPECT_EQ(123456789u, d.offset());
  EXPECT_EQ(4096u, d.size());
}

TEST(FormatTest, FooterRoundTripAndMagic) {
  Footer f;
  BlockHandle a;
  a.set_offset(1);
  a.set_size(2);
  BlockHandle b;
  b.set_offset(3);
  b.set_size(4);
  f.set_filter_handle(a);
  f.set_index_handle(b);
  std::string enc;
  f.EncodeTo(&enc);
  ASSERT_EQ(Footer::kEncodedLength, enc.size());
  Footer g;
  Slice in(enc);
  ASSERT_TRUE(g.DecodeFrom(&in).ok());
  EXPECT_EQ(3u, g.index_handle().offset());
  EXPECT_EQ(2u, g.filter_handle().size());

  enc[enc.size() - 1] ^= 0x1;  // break the magic number
  Slice bad(enc);
  Status s = g.DecodeFrom(&bad);
  EXPECT_TRUE(s.IsCorruption());
}

class BlockTest : public ::testing::TestWithParam<int> {};

TEST_P(BlockTest, RoundTripAndSeekMatchesStdMap) {
  const int restart_interval = GetParam();
  const Comparator* cmp = BytewiseComparator();
  Random rnd(static_cast<uint64_t>(restart_interval));
  std::map<std::string, std::string> model;
  for (int i = 0; i < 2000; i++) {
    // Shared prefixes make prefix compression do real work.
    model["user:" + test::Key(rnd.Uniform(100000))] =
        test::RandomString(&rnd, rnd.Uniform(50));
  }
  BlockBuilder builder(cmp, restart_interval);
  for (const auto& [k, v] : model) builder.Add(k, v);
  const Slice raw = builder.Finish();
  auto block = std::make_shared<const Block>(raw.ToString());
  ASSERT_FALSE(block->malformed());

  // Full scan.
  auto it = NewBlockIterator(block, cmp);
  it->SeekToFirst();
  for (const auto& [k, v] : model) {
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(k, it->key().ToString());
    ASSERT_EQ(v, it->value().ToString());
    it->Next();
  }
  ASSERT_FALSE(it->Valid());
  ASSERT_TRUE(it->status().ok());

  // Seek to random targets (present and absent).
  for (int i = 0; i < 2000; i++) {
    const std::string target = "user:" + test::Key(rnd.Uniform(100001));
    it->Seek(target);
    auto m = model.lower_bound(target);
    if (m == model.end()) {
      ASSERT_FALSE(it->Valid());
    } else {
      ASSERT_TRUE(it->Valid());
      ASSERT_EQ(m->first, it->key().ToString());
    }
  }
}

INSTANTIATE_TEST_SUITE_P(RestartIntervals, BlockTest,
                         ::testing::Values(1, 2, 16, 1024));

TEST(BlockTest, PrefixCompressionShrinksSortedKeys) {
  BlockBuilder with(BytewiseComparator(), 16);
  BlockBuilder without(BytewiseComparator(), 1);
  for (int i = 0; i < 500; i++) {
    const std::string k = "a/fairly/long/common/prefix/" + test::Key(i);
    with.Add(k, "v");
    without.Add(k, "v");
  }
  const size_t compressed = with.Finish().size();
  const size_t plain = without.Finish().size();
  EXPECT_LT(compressed * 2, plain);  // better than 2x on this key shape
}

TEST(BlockTest, MalformedBlockIsDetected) {
  auto block = std::make_shared<const Block>(std::string("\x01\x02", 2));
  EXPECT_TRUE(block->malformed());
  auto it = NewBlockIterator(block, BytewiseComparator());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

}  // namespace lsmkv
