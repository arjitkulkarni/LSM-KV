// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_TABLE_MERGER_H_
#define LSMKV_SRC_TABLE_MERGER_H_

#include <memory>
#include <vector>

#include "lsmkv/iterator.h"

namespace lsmkv {

class Comparator;

// Returns an iterator that yields the union of `children` in sorted order,
// using a binary min-heap keyed on each child's current key: O(log k) per
// Next() for k children. Equal keys are yielded in child order, so callers
// list their newest source first.
//
// Takes ownership of the children.
std::unique_ptr<Iterator> NewMergingIterator(
    const Comparator* comparator,
    std::vector<std::unique_ptr<Iterator>> children);

}  // namespace lsmkv

#endif  // LSMKV_SRC_TABLE_MERGER_H_
