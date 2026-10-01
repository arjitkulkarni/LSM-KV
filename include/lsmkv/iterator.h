// Copyright (c) LSM-KV authors. Licensed under the MIT license.
//
// Iterator: a forward cursor over a sorted sequence of key/value pairs.

#ifndef LSMKV_INCLUDE_LSMKV_ITERATOR_H_
#define LSMKV_INCLUDE_LSMKV_ITERATOR_H_

#include <memory>

#include "lsmkv/slice.h"
#include "lsmkv/status.h"

namespace lsmkv {

// The same interface is implemented by a memtable cursor, an SSTable block
// cursor, a two-level (index -> block) cursor, a k-way merging cursor and
// the user-facing DB cursor. That uniformity is what lets compaction, flush
// and range scans all be written once against "an Iterator".
//
// Iterators are forward-only by design (see README "Limitations").
// An iterator owns whatever it needs to stay valid (memtables, table
// handles, cached blocks) through shared_ptrs, so it can safely outlive a
// flush or compaction that retires the files it reads from.
class Iterator {
 public:
  Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  virtual ~Iterator();

  virtual bool Valid() const = 0;

  // Positions at the first entry. Valid() iff the source is non-empty.
  virtual void SeekToFirst() = 0;

  // Positions at the first entry with key >= target.
  virtual void Seek(const Slice& target) = 0;

  // REQUIRES: Valid().
  virtual void Next() = 0;

  // REQUIRES: Valid(). The returned slices are only valid until the next
  // modification of the iterator.
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;

  // Non-OK if an error (I/O, corruption) was encountered. A cursor that hits
  // an error becomes !Valid() rather than yielding garbage.
  virtual Status status() const = 0;
};

// An iterator over nothing, optionally carrying an error status.
std::unique_ptr<Iterator> NewEmptyIterator();
std::unique_ptr<Iterator> NewErrorIterator(const Status& status);

}  // namespace lsmkv

#endif  // LSMKV_INCLUDE_LSMKV_ITERATOR_H_
