// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "util/crc32c.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace lsmkv::crc32c {

// Test vectors from RFC 3720 section B.4 (iSCSI).
TEST(CRC32CTest, StandardResults) {
  char buf[32];

  std::memset(buf, 0, sizeof(buf));
  EXPECT_EQ(0x8a9136aau, Value(buf, sizeof(buf)));

  std::memset(buf, 0xff, sizeof(buf));
  EXPECT_EQ(0x62a8ab43u, Value(buf, sizeof(buf)));

  for (int i = 0; i < 32; i++) buf[i] = static_cast<char>(i);
  EXPECT_EQ(0x46dd794eu, Value(buf, sizeof(buf)));

  for (int i = 0; i < 32; i++) buf[i] = static_cast<char>(31 - i);
  EXPECT_EQ(0x113fdb5cu, Value(buf, sizeof(buf)));

  EXPECT_EQ(0xe3069283u, Value("123456789", 9));
}

TEST(CRC32CTest, Values) { EXPECT_NE(Value("a", 1), Value("foo", 3)); }

TEST(CRC32CTest, Extend) {
  EXPECT_EQ(Value("hello world", 11), Extend(Value("hello ", 6), "world", 5));
}

TEST(CRC32CTest, ExtendAtEveryAlignment) {
  // Exercises the slicing-by-8 main loop and the byte-at-a-time tail at
  // every split point.
  std::string data;
  for (int i = 0; i < 1000; i++) data.push_back(static_cast<char>(i * 7 + 3));
  const uint32_t whole = Value(data.data(), data.size());
  for (size_t split = 0; split <= data.size(); split += 13) {
    EXPECT_EQ(whole, Extend(Value(data.data(), split), data.data() + split,
                            data.size() - split));
  }
}

TEST(CRC32CTest, Mask) {
  const uint32_t crc = Value("foo", 3);
  EXPECT_NE(crc, Mask(crc));
  EXPECT_NE(crc, Mask(Mask(crc)));
  EXPECT_EQ(crc, Unmask(Mask(crc)));
  EXPECT_EQ(crc, Unmask(Unmask(Mask(Mask(crc)))));
}

}  // namespace lsmkv::crc32c
