// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "table/format.h"

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsmkv {

void BlockHandle::EncodeTo(std::string* dst) const {
  // Sanity check that all fields have been set.
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &offset_) && GetVarint64(input, &size_)) {
    return Status::OK();
  }
  return Status::Corruption("bad block handle");
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();
  filter_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  dst->resize(original_size + 2 * BlockHandle::kMaxEncodedLength);  // pad
  PutFixed64(dst, kTableMagicNumber);
}

Status Footer::DecodeFrom(Slice* input) {
  if (input->size() < kEncodedLength) {
    return Status::Corruption("footer too short");
  }
  const char* magic_ptr = input->data() + kEncodedLength - 8;
  const uint64_t magic = DecodeFixed64(magic_ptr);
  if (magic != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic number)");
  }
  Status result = filter_handle_.DecodeFrom(input);
  if (result.ok()) result = index_handle_.DecodeFrom(input);
  if (result.ok()) {
    // Skip over any leftover data (padding) in the input.
    const char* end = magic_ptr + 8;
    *input = Slice(end, static_cast<size_t>(input->data() + input->size() - end));
  }
  return result;
}

Status ReadBlock(const RandomAccessFile* file, const BlockHandle& handle,
                 std::string* contents) {
  contents->clear();
  const auto n = static_cast<size_t>(handle.size());
  std::string buf;
  buf.resize(n + kBlockTrailerSize);
  Slice slice;
  Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &slice, &buf[0]);
  if (!s.ok()) return s;
  if (slice.size() != n + kBlockTrailerSize) {
    return Status::Corruption("truncated block read");
  }

  // Every block read from the file is checksummed before it is trusted.
  const char* data = slice.data();
  const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
  const uint32_t actual = crc32c::Value(data, n + 1);
  if (actual != crc) {
    return Status::Corruption("block checksum mismatch");
  }
  if (data[n] != kNoCompression) {
    return Status::Corruption("unknown block compression type");
  }
  if (data != buf.data()) {
    contents->assign(data, n);
  } else {
    buf.resize(n);
    contents->swap(buf);
  }
  return Status::OK();
}

}  // namespace lsmkv
