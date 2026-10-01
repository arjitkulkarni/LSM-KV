// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "util/coding.h"

namespace lsmkv {

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

char* EncodeVarint32(char* dst, uint32_t v) {
  auto* ptr = reinterpret_cast<uint8_t*>(dst);
  constexpr uint32_t B = 128;
  while (v >= B) {
    *(ptr++) = static_cast<uint8_t>(v | B);
    v >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

char* EncodeVarint64(char* dst, uint64_t v) {
  auto* ptr = reinterpret_cast<uint8_t*>(dst);
  constexpr uint64_t B = 128;
  while (v >= B) {
    *(ptr++) = static_cast<uint8_t>(v | B);
    v >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

void PutVarint32(std::string* dst, uint32_t v) {
  char buf[kMaxVarint32Bytes];
  char* ptr = EncodeVarint32(buf, v);
  dst->append(buf, static_cast<size_t>(ptr - buf));
}

void PutVarint64(std::string* dst, uint64_t v) {
  char buf[kMaxVarint64Bytes];
  char* ptr = EncodeVarint64(buf, v);
  dst->append(buf, static_cast<size_t>(ptr - buf));
}

void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 128) {
    v >>= 7;
    len++;
  }
  return len;
}

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    const uint32_t byte = *reinterpret_cast<const uint8_t*>(p);
    p++;
    if (byte & 128) {
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return p;
    }
  }
  return nullptr;
}

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    const uint64_t byte = *reinterpret_cast<const uint8_t*>(p);
    p++;
    if (byte & 128) {
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return p;
    }
  }
  return nullptr;
}

bool GetFixed32(Slice* input, uint32_t* value) {
  if (input->size() < sizeof(uint32_t)) return false;
  *value = DecodeFixed32(input->data());
  input->remove_prefix(sizeof(uint32_t));
  return true;
}

bool GetFixed64(Slice* input, uint64_t* value) {
  if (input->size() < sizeof(uint64_t)) return false;
  *value = DecodeFixed64(input->data());
  input->remove_prefix(sizeof(uint64_t));
  return true;
}

bool GetVarint32(Slice* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) return false;
  *input = Slice(q, static_cast<size_t>(limit - q));
  return true;
}

bool GetVarint64(Slice* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, value);
  if (q == nullptr) return false;
  *input = Slice(q, static_cast<size_t>(limit - q));
  return true;
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len;
  if (GetVarint32(input, &len) && input->size() >= len) {
    *result = Slice(input->data(), len);
    input->remove_prefix(len);
    return true;
  }
  return false;
}

}  // namespace lsmkv
