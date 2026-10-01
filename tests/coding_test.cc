// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "util/coding.h"

#include <gtest/gtest.h>

#include <vector>

namespace lsmkv {

TEST(CodingTest, Fixed32RoundTrip) {
  std::string s;
  for (uint32_t v = 0; v < 100000; v++) PutFixed32(&s, v);
  const char* p = s.data();
  for (uint32_t v = 0; v < 100000; v++) {
    EXPECT_EQ(v, DecodeFixed32(p));
    p += sizeof(uint32_t);
  }
}

TEST(CodingTest, Fixed64RoundTrip) {
  std::string s;
  for (int power = 0; power <= 63; power++) {
    const uint64_t v = uint64_t{1} << power;
    PutFixed64(&s, v - 1);
    PutFixed64(&s, v + 0);
    PutFixed64(&s, v + 1);
  }
  const char* p = s.data();
  for (int power = 0; power <= 63; power++) {
    const uint64_t v = uint64_t{1} << power;
    EXPECT_EQ(v - 1, DecodeFixed64(p));
    p += 8;
    EXPECT_EQ(v + 0, DecodeFixed64(p));
    p += 8;
    EXPECT_EQ(v + 1, DecodeFixed64(p));
    p += 8;
  }
}

TEST(CodingTest, EncodingIsLittleEndian) {
  std::string dst;
  PutFixed32(&dst, 0x04030201);
  ASSERT_EQ(4u, dst.size());
  EXPECT_EQ(0x01, static_cast<int>(dst[0]));
  EXPECT_EQ(0x04, static_cast<int>(dst[3]));
}

TEST(CodingTest, Varint32RoundTrip) {
  std::string s;
  for (uint32_t i = 0; i < (32 * 32); i++) {
    const uint32_t v = (i / 32) << (i % 32);
    PutVarint32(&s, v);
  }
  const char* p = s.data();
  const char* limit = p + s.size();
  for (uint32_t i = 0; i < (32 * 32); i++) {
    const uint32_t expected = (i / 32) << (i % 32);
    uint32_t actual;
    const char* start = p;
    p = GetVarint32Ptr(p, limit, &actual);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(expected, actual);
    EXPECT_EQ(VarintLength(actual), p - start);
  }
  EXPECT_EQ(p, s.data() + s.size());
}

TEST(CodingTest, Varint64RoundTrip) {
  std::vector<uint64_t> values = {0, 100, ~uint64_t{0}, ~uint64_t{0} - 1};
  for (uint32_t k = 0; k < 64; k++) {
    const uint64_t power = uint64_t{1} << k;
    values.push_back(power);
    values.push_back(power - 1);
    values.push_back(power + 1);
  }
  std::string s;
  for (uint64_t v : values) PutVarint64(&s, v);
  const char* p = s.data();
  const char* limit = p + s.size();
  for (uint64_t expected : values) {
    uint64_t actual;
    const char* start = p;
    p = GetVarint64Ptr(p, limit, &actual);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(expected, actual);
    EXPECT_EQ(VarintLength(actual), p - start);
  }
}

TEST(CodingTest, Varint32Overflow) {
  uint32_t result;
  const std::string input("\x81\x82\x83\x84\x85\x11");
  EXPECT_EQ(nullptr,
            GetVarint32Ptr(input.data(), input.data() + input.size(), &result));
}

TEST(CodingTest, Varint32Truncation) {
  const uint32_t large_value = (1u << 31) + 100;
  std::string s;
  PutVarint32(&s, large_value);
  uint32_t result;
  for (size_t len = 0; len < s.size() - 1; len++) {
    EXPECT_EQ(nullptr, GetVarint32Ptr(s.data(), s.data() + len, &result));
  }
  EXPECT_NE(nullptr, GetVarint32Ptr(s.data(), s.data() + s.size(), &result));
  EXPECT_EQ(large_value, result);
}

TEST(CodingTest, LengthPrefixedSlices) {
  std::string s;
  PutLengthPrefixedSlice(&s, Slice(""));
  PutLengthPrefixedSlice(&s, Slice("foo"));
  PutLengthPrefixedSlice(&s, Slice("bar"));
  PutLengthPrefixedSlice(&s, Slice(std::string(200, 'x')));

  Slice input(s);
  Slice v;
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  EXPECT_EQ("", v.ToString());
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  EXPECT_EQ("foo", v.ToString());
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  EXPECT_EQ("bar", v.ToString());
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  EXPECT_EQ(std::string(200, 'x'), v.ToString());
  EXPECT_EQ("", input.ToString());
}

}  // namespace lsmkv
