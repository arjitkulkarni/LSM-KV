// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// CRC-32C (Castagnoli polynomial 0x1EDC6F41), the checksum used by iSCSI,
// ext4 metadata and most LSM engines. It detects all burst errors up to 32
// bits and has hardware support on modern x86 and ARM.

#ifndef LSMKV_SRC_UTIL_CRC32C_H_
#define LSMKV_SRC_UTIL_CRC32C_H_

#include <cstddef>
#include <cstdint>

namespace lsmkv::crc32c {

// Returns the crc32c of concat(A, data[0,n-1]) where init_crc is the
// crc32c of some string A. Extend(0, ...) computes a fresh checksum.
uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

// Checksums are "masked" before being stored: computing the CRC of a string
// that itself contains embedded CRCs is otherwise prone to degenerate cases
// (e.g. a CRC over zeroes stored next to zeroes).
constexpr uint32_t kMaskDelta = 0xa282ead8ul;

inline uint32_t Mask(uint32_t crc) {
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

inline uint32_t Unmask(uint32_t masked_crc) {
  const uint32_t rot = masked_crc - kMaskDelta;
  return ((rot >> 17) | (rot << 15));
}

}  // namespace lsmkv::crc32c

#endif  // LSMKV_SRC_UTIL_CRC32C_H_
