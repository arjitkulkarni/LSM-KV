// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_TABLE_TWO_LEVEL_ITERATOR_H_
#define LSMKV_SRC_TABLE_TWO_LEVEL_ITERATOR_H_

#include <functional>
#include <memory>

#include "lsmkv/iterator.h"

namespace lsmkv {

// Maps the value of an index entry (e.g. an encoded BlockHandle, or a file
// number) to an iterator over the data it points at.
using BlockFunction = std::function<std::unique_ptr<Iterator>(const Slice&)>;

// A two-level iterator walks an index iterator whose values point at
// "blocks", opening each block lazily with `block_function`. Used twice:
//   * within an SSTable: index block -> data blocks
//   * within a level:    sorted file list -> SSTables
std::unique_ptr<Iterator> NewTwoLevelIterator(
    std::unique_ptr<Iterator> index_iter, BlockFunction block_function);

}  // namespace lsmkv

#endif  // LSMKV_SRC_TABLE_TWO_LEVEL_ITERATOR_H_
