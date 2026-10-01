// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Endian-neutral encoding:
//   * Fixed-length integers are little-endian.
//   * Varints use 7 bits per byte, high bit = "more bytes follow".

#ifndef LSMKV_SRC_UTIL_CODING_H_
#define LSMKV_SRC_UTIL_CODING_H_

#include <cstdint>
#include <cstring>
#include <string>

#include "lsmkv/slice.h"

namespace lsmkv {

constexpr int kMaxVarint32Bytes = 5;
constexpr int kMaxVarint64Bytes = 10;

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);
void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);

// Each Get* consumes the decoded bytes from the front of *input and returns
// false (leaving *input unspecified) if the encoding is truncated or invalid.
bool GetFixed32(Slice* input, uint32_t* value);
bool GetFixed64(Slice* input, uint64_t* value);
bool GetVarint32(Slice* input, uint32_t* value);
bool GetVarint64(Slice* input, uint64_t* value);
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

// Pointer-based decoders: return a pointer just past the parsed value, or
// nullptr on error. Never read at or beyond `limit`.
const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* v);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* v);

int VarintLength(uint64_t v);

// Writes directly into dst; returns a pointer past the last byte written.
char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);

inline void EncodeFixed32(char* dst, uint32_t value) {
  auto* const buf = reinterpret_cast<uint8_t*>(dst);
  buf[0] = static_cast<uint8_t>(value);
  buf[1] = static_cast<uint8_t>(value >> 8);
  buf[2] = static_cast<uint8_t>(value >> 16);
  buf[3] = static_cast<uint8_t>(value >> 24);
}

inline void EncodeFixed64(char* dst, uint64_t value) {
  auto* const buf = reinterpret_cast<uint8_t*>(dst);
  for (int i = 0; i < 8; ++i) {
    buf[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

inline uint32_t DecodeFixed32(const char* ptr) {
  const auto* const buf = reinterpret_cast<const uint8_t*>(ptr);
  return static_cast<uint32_t>(buf[0]) |
         (static_cast<uint32_t>(buf[1]) << 8) |
         (static_cast<uint32_t>(buf[2]) << 16) |
         (static_cast<uint32_t>(buf[3]) << 24);
}

inline uint64_t DecodeFixed64(const char* ptr) {
  const auto* const buf = reinterpret_cast<const uint8_t*>(ptr);
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) {
    v = (v << 8) | buf[i];
  }
  return v;
}

// Fast path for the overwhelmingly common one-byte varint.
inline const char* GetVarint32PtrFast(const char* p, const char* limit,
                                      uint32_t* value) {
  if (p < limit) {
    const uint32_t result = *reinterpret_cast<const uint8_t*>(p);
    if ((result & 128) == 0) {
      *value = result;
      return p + 1;
    }
  }
  return GetVarint32Ptr(p, limit, value);
}

}  // namespace lsmkv

#endif  // LSMKV_SRC_UTIL_CODING_H_
