// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Status: the result of every fallible operation in the public API.

#ifndef LSMKV_INCLUDE_LSMKV_STATUS_H_
#define LSMKV_INCLUDE_LSMKV_STATUS_H_

#include <string>
#include <utility>

#include "lsmkv/slice.h"

namespace lsmkv {

// Status is a value type (rule of zero). The class is [[nodiscard]] so that
// silently dropping an I/O error is a compiler warning, not a latent
// durability bug. Callers that genuinely do not care must say so with a
// (void) cast, which keeps the decision visible in review.
class [[nodiscard]] Status {
 public:
  enum class Code : unsigned char {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kNotSupported = 3,
    kInvalidArgument = 4,
    kIOError = 5,
  };

  Status() noexcept = default;

  static Status OK() noexcept { return Status(); }
  static Status NotFound(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(Code::kNotFound, msg, msg2);
  }
  static Status Corruption(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(Code::kCorruption, msg, msg2);
  }
  static Status NotSupported(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(Code::kNotSupported, msg, msg2);
  }
  static Status InvalidArgument(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(Code::kInvalidArgument, msg, msg2);
  }
  static Status IOError(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(Code::kIOError, msg, msg2);
  }

  bool ok() const noexcept { return code_ == Code::kOk; }
  bool IsNotFound() const noexcept { return code_ == Code::kNotFound; }
  bool IsCorruption() const noexcept { return code_ == Code::kCorruption; }
  bool IsIOError() const noexcept { return code_ == Code::kIOError; }
  bool IsNotSupported() const noexcept { return code_ == Code::kNotSupported; }
  bool IsInvalidArgument() const noexcept {
    return code_ == Code::kInvalidArgument;
  }

  Code code() const noexcept { return code_; }
  const std::string& message() const noexcept { return msg_; }

  // Human readable form, e.g. "Corruption: bad record checksum".
  std::string ToString() const;

 private:
  Status(Code code, const Slice& msg, const Slice& msg2);

  Code code_ = Code::kOk;
  std::string msg_;
};

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_STATUS_H_
