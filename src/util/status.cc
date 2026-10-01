// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "lsmkv/status.h"

namespace lsmkv {

Status::Status(Code code, const Slice& msg, const Slice& msg2) : code_(code) {
  msg_.assign(msg.data(), msg.size());
  if (!msg2.empty()) {
    msg_.append(": ");
    msg_.append(msg2.data(), msg2.size());
  }
}

std::string Status::ToString() const {
  const char* type = "";
  switch (code_) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      type = "NotFound: ";
      break;
    case Code::kCorruption:
      type = "Corruption: ";
      break;
    case Code::kNotSupported:
      type = "Not implemented: ";
      break;
    case Code::kInvalidArgument:
      type = "Invalid argument: ";
      break;
    case Code::kIOError:
      type = "IO error: ";
      break;
  }
  return std::string(type) + msg_;
}

}  // namespace lsmkv
