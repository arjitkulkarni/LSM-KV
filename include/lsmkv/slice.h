// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Slice: a non-owning, trivially-copyable view of a byte range.

#ifndef LSMKV_INCLUDE_LSMKV_SLICE_H_
#define LSMKV_INCLUDE_LSMKV_SLICE_H_

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace lsmkv {

// A Slice points at bytes owned by someone else. Copying a Slice copies two
// words, never the payload, so the referenced storage must outlive every
// Slice that points into it. Keys and values are arbitrary bytes (embedded
// NULs are fine), which is why this is not simply a std::string_view alias:
// comparisons are always unsigned bytewise.
class Slice {
 public:
  constexpr Slice() noexcept : data_(""), size_(0) {}
  constexpr Slice(const char* d, size_t n) noexcept : data_(d), size_(n) {}
  Slice(const std::string& s) noexcept  // NOLINT(google-explicit-constructor)
      : data_(s.data()), size_(s.size()) {}
  constexpr Slice(std::string_view s) noexcept  // NOLINT(google-explicit-constructor)
      : data_(s.data()), size_(s.size()) {}
  Slice(const char* s) noexcept  // NOLINT(google-explicit-constructor)
      : data_(s), size_(std::strlen(s)) {}

  const char* data() const noexcept { return data_; }
  size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }

  char operator[](size_t n) const {
    assert(n < size_);
    return data_[n];
  }

  void clear() noexcept {
    data_ = "";
    size_ = 0;
  }

  void remove_prefix(size_t n) {
    assert(n <= size_);
    data_ += n;
    size_ -= n;
  }

  std::string ToString() const { return std::string(data_, size_); }
  std::string_view ToStringView() const noexcept { return {data_, size_}; }

  // Three-way unsigned bytewise comparison: <0, 0, >0.
  int compare(const Slice& b) const noexcept;

  bool starts_with(const Slice& x) const noexcept {
    return size_ >= x.size_ &&
           (x.size_ == 0 || std::memcmp(data_, x.data_, x.size_) == 0);
  }

 private:
  const char* data_;
  size_t size_;
};

inline bool operator==(const Slice& a, const Slice& b) noexcept {
  return a.size() == b.size() &&
         (a.size() == 0 || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

inline bool operator!=(const Slice& a, const Slice& b) noexcept {
  return !(a == b);
}

inline int Slice::compare(const Slice& b) const noexcept {
  const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
  int r = (min_len == 0) ? 0 : std::memcmp(data_, b.data_, min_len);
  if (r == 0) {
    if (size_ < b.size_) {
      r = -1;
    } else if (size_ > b.size_) {
      r = +1;
    }
  }
  return r;
}

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_SLICE_H_
