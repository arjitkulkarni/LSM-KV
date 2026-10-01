// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "lsmkv/write_batch.h"

#include <gtest/gtest.h>

#include "db/write_batch_internal.h"
#include "memtable/memtable.h"

namespace lsmkv {

namespace {

std::string PrintContents(WriteBatch* b) {
  InternalKeyComparator cmp(BytewiseComparator());
  auto mem = std::make_shared<MemTable>(cmp, 1);
  std::string state;
  Status s = WriteBatchInternal::InsertInto(b, mem.get());
  int count = 0;
  auto iter = mem->NewIterator();
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey ikey;
    EXPECT_TRUE(ParseInternalKey(iter->key(), &ikey));
    switch (ikey.type) {
      case kTypeValue:
        state += "Put(" + ikey.user_key.ToString() + ", " +
                 iter->value().ToString() + ")";
        count++;
        break;
      case kTypeDeletion:
        state += "Delete(" + ikey.user_key.ToString() + ")";
        count++;
        break;
    }
    state += "@" + std::to_string(ikey.sequence);
  }
  if (!s.ok()) {
    state += "ParseError()";
  } else if (count != static_cast<int>(WriteBatchInternal::Count(b))) {
    state += "CountMismatch()";
  }
  return state;
}

}  // namespace

TEST(WriteBatchTest, Empty) {
  WriteBatch batch;
  EXPECT_EQ("", PrintContents(&batch));
  EXPECT_EQ(0u, batch.Count());
}

TEST(WriteBatchTest, Multiple) {
  WriteBatch batch;
  batch.Put("foo", "bar");
  batch.Delete("box");
  batch.Put("baz", "boo");
  WriteBatchInternal::SetSequence(&batch, 100);
  EXPECT_EQ(100u, WriteBatchInternal::Sequence(&batch));
  EXPECT_EQ(3u, batch.Count());
  EXPECT_EQ(
      "Put(baz, boo)@102"
      "Delete(box)@101"
      "Put(foo, bar)@100",
      PrintContents(&batch));
}

TEST(WriteBatchTest, Corruption) {
  WriteBatch batch;
  batch.Put("foo", "bar");
  batch.Delete("box");
  WriteBatchInternal::SetSequence(&batch, 200);
  const Slice contents = WriteBatchInternal::Contents(&batch);
  WriteBatchInternal::SetContents(&batch,
                                  Slice(contents.data(), contents.size() - 1));
  EXPECT_EQ("Put(foo, bar)@200ParseError()", PrintContents(&batch));
}

TEST(WriteBatchTest, Append) {
  WriteBatch b1;
  WriteBatch b2;
  WriteBatchInternal::SetSequence(&b1, 200);
  WriteBatchInternal::SetSequence(&b2, 300);
  b1.Append(b2);
  EXPECT_EQ("", PrintContents(&b1));
  b2.Put("a", "va");
  b1.Append(b2);
  EXPECT_EQ("Put(a, va)@200", PrintContents(&b1));
  b2.Clear();
  b2.Put("b", "vb");
  b1.Append(b2);
  EXPECT_EQ("Put(a, va)@200Put(b, vb)@201", PrintContents(&b1));
  b2.Delete("foo");
  b1.Append(b2);
  EXPECT_EQ("Put(a, va)@200Put(b, vb)@202Put(b, vb)@201Delete(foo)@203",
            PrintContents(&b1));
}

TEST(WriteBatchTest, ApproximateSize) {
  WriteBatch batch;
  const size_t empty_size = batch.ApproximateSize();
  batch.Put("foo", "bar");
  const size_t one_key_size = batch.ApproximateSize();
  EXPECT_LT(empty_size, one_key_size);
  batch.Put("baz", "boo");
  EXPECT_LT(one_key_size, batch.ApproximateSize());
}

}  // namespace lsmkv
