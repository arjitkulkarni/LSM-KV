// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "wal/log_reader.h"

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsmkv::log {

Reader::Reporter::~Reporter() = default;

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      backing_store_(new char[kBlockSize]) {}

Reader::~Reader() = default;

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  scratch->clear();
  record->clear();
  bool in_fragmented_record = false;
  // Offset of the logical record currently being assembled.
  uint64_t prospective_record_offset = 0;

  Slice fragment;
  while (true) {
    const unsigned int record_type = ReadPhysicalRecord(&fragment);

    // Offset of the physical record just returned (valid for real types).
    const uint64_t physical_record_offset =
        end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();
    const uint64_t physical_record_end = end_of_buffer_offset_ - buffer_.size();

    switch (record_type) {
      case kFullType:
        if (in_fragmented_record && !scratch->empty()) {
          ReportCorruption(scratch->size(), "partial record without end (1)");
        }
        scratch->clear();
        *record = fragment;
        last_record_offset_ = physical_record_offset;
        last_record_end_offset_ = physical_record_end;
        return true;

      case kFirstType:
        if (in_fragmented_record && !scratch->empty()) {
          ReportCorruption(scratch->size(), "partial record without end (2)");
        }
        prospective_record_offset = physical_record_offset;
        scratch->assign(fragment.data(), fragment.size());
        in_fragmented_record = true;
        break;

      case kMiddleType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(),
                           "missing start of fragmented record (1)");
        } else {
          scratch->append(fragment.data(), fragment.size());
        }
        break;

      case kLastType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(),
                           "missing start of fragmented record (2)");
        } else {
          scratch->append(fragment.data(), fragment.size());
          *record = Slice(*scratch);
          last_record_offset_ = prospective_record_offset;
          last_record_end_offset_ = physical_record_end;
          return true;
        }
        break;

      case kEof:
        if (in_fragmented_record) {
          // The writer died between fragments of one record: a torn tail,
          // not corruption.
          truncated_tail_ = true;
          scratch->clear();
        }
        return false;

      case kBadRecord:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "error in middle of record");
          in_fragmented_record = false;
          scratch->clear();
        }
        break;

      default:
        ReportCorruption(
            fragment.size() + (in_fragmented_record ? scratch->size() : 0),
            "unknown record type");
        in_fragmented_record = false;
        scratch->clear();
        break;
    }
  }
}

void Reader::ReportCorruption(uint64_t bytes, const char* reason) {
  ReportDrop(bytes, Status::Corruption(reason));
}

void Reader::ReportDrop(uint64_t bytes, const Status& reason) {
  if (reporter_ != nullptr) {
    reporter_->Corruption(static_cast<size_t>(bytes), reason);
  }
}

unsigned int Reader::ReadPhysicalRecord(Slice* result) {
  while (true) {
    if (buffer_.size() < static_cast<size_t>(kHeaderSize)) {
      if (!eof_) {
        // The previous read consumed a whole block; what is left is the
        // zero trailer. Skip it and read the next block.
        buffer_.clear();
        Status status = file_->Read(kBlockSize, &buffer_, backing_store_.get());
        end_of_buffer_offset_ += buffer_.size();
        if (!status.ok()) {
          buffer_.clear();
          ReportDrop(kBlockSize, status);
          eof_ = true;
          return kEof;
        }
        if (buffer_.size() < static_cast<size_t>(kBlockSize)) eof_ = true;
        continue;
      }
      // EOF with a partial header left: the writer died mid-header.
      if (!buffer_.empty()) truncated_tail_ = true;
      buffer_.clear();
      return kEof;
    }

    // Parse the header.
    const char* header = buffer_.data();
    const uint32_t a = static_cast<uint32_t>(header[4]) & 0xff;
    const uint32_t b = static_cast<uint32_t>(header[5]) & 0xff;
    const unsigned int type = static_cast<unsigned char>(header[6]);
    const uint32_t length = a | (b << 8);

    if (kHeaderSize + length > buffer_.size()) {
      const size_t drop_size = buffer_.size();
      buffer_.clear();
      if (!eof_) {
        ReportCorruption(drop_size, "bad record length");
        return kBadRecord;
      }
      // The record runs past EOF: the writer died mid-payload.
      truncated_tail_ = true;
      return kEof;
    }

    if (type == kZeroType && length == 0) {
      // A zero-filled header can only come from preallocated or torn space.
      // Report it: whether that is acceptable depends on whether any valid
      // record follows, which is the caller's call.
      const size_t drop_size = buffer_.size();
      buffer_.clear();
      ReportCorruption(drop_size, "zero-filled region");
      return kBadRecord;
    }

    if (checksum_) {
      const uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
      const uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
      if (actual_crc != expected_crc) {
        // Drop the rest of the block: `length` itself may be the corrupted
        // field, so we cannot trust it to find the next header.
        const size_t drop_size = buffer_.size();
        buffer_.clear();
        ReportCorruption(drop_size, "checksum mismatch");
        return kBadRecord;
      }
    }

    buffer_.remove_prefix(kHeaderSize + length);
    *result = Slice(header + kHeaderSize, length);
    return type;
  }
}

}  // namespace lsmkv::log
