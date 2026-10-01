// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#ifndef LSMKV_SRC_DB_DB_ITER_H_
#define LSMKV_SRC_DB_DB_ITER_H_

#include <memory>

#include "db/dbformat.h"
#include "lsmkv/iterator.h"

namespace lsmkv {

// Converts a merged stream of internal keys (user_key, sequence, type) --
// newest version of each key first -- into the user's view: one entry per
// live key, tombstones and shadowed versions hidden, and entries newer than
// `snapshot` invisible.
//
// `pin` keeps alive whatever the internal iterator reads from (memtables and
// the Version); it is released when the DBIter is destroyed.
std::unique_ptr<Iterator> NewDBIterator(const Comparator* user_comparator,
                                        std::unique_ptr<Iterator> internal_iter,
                                        SequenceNumber snapshot,
                                        std::shared_ptr<const void> pin);

}  // namespace lsmkv

#endif  // LSMKV_SRC_DB_DB_ITER_H_
