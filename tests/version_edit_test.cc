// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "db/version_edit.h"

#include <gtest/gtest.h>

namespace lsmkv {

static void TestEncodeDecode(const VersionEdit& edit) {
  std::string encoded;
  std::string encoded2;
  edit.EncodeTo(&encoded);
  VersionEdit parsed;
  Status s = parsed.DecodeFrom(encoded);
  ASSERT_TRUE(s.ok()) << s.ToString();
  parsed.EncodeTo(&encoded2);
  ASSERT_EQ(encoded, encoded2);
}

TEST(VersionEditTest, EncodeDecode) {
  constexpr uint64_t kBig = 1ull << 50;

  VersionEdit edit;
  for (int i = 0; i < 4; i++) {
    TestEncodeDecode(edit);
    edit.AddFile(3, kBig + 300 + i, kBig + 400 + i,
                 InternalKey("foo", kBig + 500 + i, kTypeValue),
                 InternalKey("zoo", kBig + 600 + i, kTypeDeletion));
    edit.RemoveFile(4, kBig + 700 + i);
    edit.SetCompactPointer(i, InternalKey("x", kBig + 900 + i, kTypeValue));
  }

  edit.SetComparatorName("foo");
  edit.SetLogNumber(kBig + 100);
  edit.SetNextFile(kBig + 200);
  edit.SetLastSequence(kBig + 1000);
  TestEncodeDecode(edit);
}

TEST(VersionEditTest, RejectsGarbage) {
  VersionEdit edit;
  EXPECT_TRUE(edit.DecodeFrom(Slice("\x07\x09", 2)).IsCorruption());
  EXPECT_TRUE(edit.DecodeFrom(Slice("\x63", 1)).IsCorruption());  // tag 99
}

}  // namespace lsmkv
