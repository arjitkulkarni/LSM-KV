// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include <gtest/gtest.h>

#include "test_util.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/random.h"
#include "wal/log_reader.h"
#include "wal/log_writer.h"

namespace lsmkv::log {

namespace {

// Construct a string of the specified length made out of the supplied
// partial string.
std::string BigString(const std::string& partial_string, size_t n) {
  std::string result;
  while (result.size() < n) result.append(partial_string);
  result.resize(n);
  return result;
}

std::string NumberString(int n) { return std::to_string(n) + "."; }

class CountingReporter final : public Reader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& status) override {
    dropped_bytes += bytes;
    count++;
    message = status.ToString();
  }
  size_t dropped_bytes = 0;
  int count = 0;
  std::string message;
};

class LogTest : public ::testing::Test {
 protected:
  LogTest() : writer_(std::make_unique<Writer>(&dest_)) {}

  void Write(const std::string& msg) { ASSERT_TRUE(writer_->AddRecord(msg).ok()); }

  size_t WrittenBytes() { return dest_.contents().size(); }

  std::string Read() {
    if (reader_ == nullptr) {
      source_ = std::make_unique<test::StringSource>(dest_.contents());
      reader_ = std::make_unique<Reader>(source_.get(), &report_, true);
    }
    std::string scratch;
    Slice record;
    if (reader_->ReadRecord(&record, &scratch)) return record.ToString();
    return "EOF";
  }

  void IncrementByte(size_t offset, int delta) {
    dest_.contents()[offset] = static_cast<char>(dest_.contents()[offset] + delta);
  }
  void SetByte(size_t offset, char new_byte) { dest_.contents()[offset] = new_byte; }
  void ShrinkSize(size_t bytes) {
    dest_.contents().resize(dest_.contents().size() - bytes);
  }
  void FixChecksum(size_t header_offset, size_t len) {
    // Compute crc of type/len/data
    uint32_t crc = crc32c::Value(&dest_.contents()[header_offset + 6], 1 + len);
    crc = crc32c::Mask(crc);
    EncodeFixed32(&dest_.contents()[header_offset], crc);
  }

  test::StringSink dest_;
  std::unique_ptr<Writer> writer_;
  std::unique_ptr<test::StringSource> source_;
  std::unique_ptr<Reader> reader_;
  CountingReporter report_;
};

}  // namespace

TEST_F(LogTest, Empty) { ASSERT_EQ("EOF", Read()); }

TEST_F(LogTest, ReadWrite) {
  Write("foo");
  Write("bar");
  Write("");
  Write("xxxx");
  ASSERT_EQ("foo", Read());
  ASSERT_EQ("bar", Read());
  ASSERT_EQ("", Read());
  ASSERT_EQ("xxxx", Read());
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ("EOF", Read());  // Make sure reads at eof work
  EXPECT_EQ(0, report_.count);
}

TEST_F(LogTest, ManyBlocks) {
  for (int i = 0; i < 100000; i++) Write(NumberString(i));
  for (int i = 0; i < 100000; i++) ASSERT_EQ(NumberString(i), Read());
  ASSERT_EQ("EOF", Read());
}

TEST_F(LogTest, Fragmentation) {
  Write("small");
  Write(BigString("medium", 50000));
  Write(BigString("large", 100000));
  ASSERT_EQ("small", Read());
  ASSERT_EQ(BigString("medium", 50000), Read());
  ASSERT_EQ(BigString("large", 100000), Read());
  ASSERT_EQ("EOF", Read());
}

TEST_F(LogTest, MarginalTrailer) {
  // Make a trailer that is exactly the same length as an empty record.
  const int n = kBlockSize - 2 * kHeaderSize;
  Write(BigString("foo", static_cast<size_t>(n)));
  ASSERT_EQ(static_cast<size_t>(kBlockSize - kHeaderSize), WrittenBytes());
  Write("");
  Write("bar");
  ASSERT_EQ(BigString("foo", static_cast<size_t>(n)), Read());
  ASSERT_EQ("", Read());
  ASSERT_EQ("bar", Read());
  ASSERT_EQ("EOF", Read());
}

TEST_F(LogTest, ShortTrailerIsPadded) {
  const int n = kBlockSize - 2 * kHeaderSize + 4;
  Write(BigString("foo", static_cast<size_t>(n)));
  ASSERT_EQ(static_cast<size_t>(kBlockSize - kHeaderSize + 4), WrittenBytes());
  Write("");
  Write("bar");
  ASSERT_EQ(BigString("foo", static_cast<size_t>(n)), Read());
  ASSERT_EQ("", Read());
  ASSERT_EQ("bar", Read());
  ASSERT_EQ("EOF", Read());
}

TEST_F(LogTest, RandomRead) {
  const int N = 500;
  Random write_rnd(301);
  for (int i = 0; i < N; i++) {
    Write(BigString(NumberString(i), write_rnd.Uniform(40000)));
  }
  Random read_rnd(301);
  for (int i = 0; i < N; i++) {
    ASSERT_EQ(BigString(NumberString(i), read_rnd.Uniform(40000)), Read());
  }
  ASSERT_EQ("EOF", Read());
}

// ---- Torn tails: not corruption ----------------------------------------------------

TEST_F(LogTest, TruncatedTrailingRecordIsTornTailNotCorruption) {
  Write("foo");
  ShrinkSize(4);  // Drop all payload as well as a header byte
  ASSERT_EQ("EOF", Read());
  EXPECT_EQ(0, report_.count);
  EXPECT_TRUE(reader_->truncated_tail());
}

TEST_F(LogTest, BadLengthAtEndIsTornTail) {
  Write("foo");
  ShrinkSize(1);
  ASSERT_EQ("EOF", Read());
  EXPECT_EQ(0, report_.count);
  EXPECT_TRUE(reader_->truncated_tail());
}

TEST_F(LogTest, TornFragmentedRecordIsTornTail) {
  Write("first");
  Write(BigString("x", 100000));  // spans 4 blocks
  ShrinkSize(20000);              // lose the LAST fragment
  ASSERT_EQ("first", Read());
  ASSERT_EQ("EOF", Read());
  EXPECT_EQ(0, report_.count);
  EXPECT_TRUE(reader_->truncated_tail());
  EXPECT_EQ(static_cast<uint64_t>(kHeaderSize + 5), reader_->LastRecordEndOffset());
}

// ---- Genuine corruption: reported --------------------------------------------------

TEST_F(LogTest, ChecksumMismatch) {
  Write("foooooo");
  IncrementByte(0, 14);
  ASSERT_EQ("EOF", Read());
  EXPECT_EQ(1, report_.count);
  EXPECT_NE(std::string::npos, report_.message.find("checksum mismatch"));
}

TEST_F(LogTest, UnexpectedMiddleType) {
  Write("foo");
  SetByte(6, kMiddleType);
  FixChecksum(0, 3);
  ASSERT_EQ("EOF", Read());
  EXPECT_EQ(3u, report_.dropped_bytes);
  EXPECT_NE(std::string::npos, report_.message.find("missing start"));
}

TEST_F(LogTest, ZeroFilledRegionIsReported) {
  Write("foo");
  test::StringSink& d = dest_;
  d.contents().append(1000, '\0');  // preallocated / torn space after it
  ASSERT_EQ("foo", Read());
  ASSERT_EQ("EOF", Read());
  EXPECT_EQ(1, report_.count);
  EXPECT_NE(std::string::npos, report_.message.find("zero-filled"));
  EXPECT_EQ(static_cast<uint64_t>(kHeaderSize + 3), reader_->LastRecordEndOffset());
}

TEST_F(LogTest, ValidRecordsAfterCorruptBlockAreStillRead) {
  // Corrupt the first block; the reader resynchronizes at the next block
  // boundary and keeps returning records. This is what lets recovery tell
  // mid-log corruption (valid data follows) from a torn tail (nothing does).
  Write(BigString("a", kBlockSize - kHeaderSize));  // exactly fills block 0
  Write("after");
  IncrementByte(100, 1);
  ASSERT_EQ("after", Read());
  EXPECT_EQ(1, report_.count);
}

TEST_F(LogTest, ReopenedWriterContinuesBlockAlignment) {
  Write("hello");
  Write(BigString("z", 40000));
  // Simulate reopening the file for append after recovery.
  writer_ = std::make_unique<Writer>(&dest_, dest_.contents().size());
  Write("world");
  Write(BigString("q", 70000));
  ASSERT_EQ("hello", Read());
  ASSERT_EQ(BigString("z", 40000), Read());
  ASSERT_EQ("world", Read());
  ASSERT_EQ(BigString("q", 70000), Read());
  ASSERT_EQ("EOF", Read());
  EXPECT_EQ(0, report_.count);
}

}  // namespace lsmkv::log
