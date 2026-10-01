// Copyright (c) LSM-KV authors. Licensed under the MIT license.

#include "db/db_iter.h"

#include <cassert>
#include <string>

namespace lsmkv {
namespace {

class DBIter final : public Iterator {
 public:
  DBIter(const Comparator* cmp, std::unique_ptr<Iterator> iter,
         SequenceNumber s, std::shared_ptr<const void> pin)
      : pin_(std::move(pin)),
        user_comparator_(cmp),
        iter_(std::move(iter)),
        sequence_(s) {}

  bool Valid() const override { return valid_; }

  Slice key() const override {
    assert(valid_);
    return ExtractUserKey(iter_->key());
  }

  Slice value() const override {
    assert(valid_);
    return iter_->value();
  }

  Status status() const override {
    if (status_.ok()) return iter_->status();
    return status_;
  }

  void Next() override {
    assert(valid_);
    // Remember the current user key so every older version of it is
    // skipped, then advance.
    saved_key_.assign(ExtractUserKey(iter_->key()).data(),
                      ExtractUserKey(iter_->key()).size());
    iter_->Next();
    FindNextUserEntry(/*skipping=*/true);
  }

  void Seek(const Slice& target) override {
    saved_key_.clear();
    AppendInternalKey(&saved_key_,
                      ParsedInternalKey(target, sequence_, kValueTypeForSeek));
    iter_->Seek(saved_key_);
    FindNextUserEntry(/*skipping=*/false);
  }

  void SeekToFirst() override {
    iter_->SeekToFirst();
    FindNextUserEntry(/*skipping=*/false);
  }

 private:
  // Advances to the next entry that is visible: newest version of a user
  // key, at or below the snapshot, and not a tombstone. When `skipping` is
  // true, entries for user keys <= saved_key_ are hidden.
  void FindNextUserEntry(bool skipping) {
    while (iter_->Valid()) {
      ParsedInternalKey ikey;
      if (!ParseInternalKey(iter_->key(), &ikey)) {
        status_ = Status::Corruption("corrupted internal key in DBIter");
        valid_ = false;
        return;
      }
      if (ikey.sequence <= sequence_) {
        switch (ikey.type) {
          case kTypeDeletion:
            // Hide this key and every older version of it.
            saved_key_.assign(ikey.user_key.data(), ikey.user_key.size());
            skipping = true;
            break;
          case kTypeValue:
            if (skipping &&
                user_comparator_->Compare(ikey.user_key, Slice(saved_key_)) <= 0) {
              // Entry hidden: an older version of a key already yielded or
              // deleted.
            } else {
              valid_ = true;
              saved_key_.clear();
              return;
            }
            break;
        }
      }
      iter_->Next();
    }
    saved_key_.clear();
    valid_ = false;
  }

  std::shared_ptr<const void> pin_;  // destroyed last-but-one: after iter_
  const Comparator* const user_comparator_;
  std::unique_ptr<Iterator> iter_;
  const SequenceNumber sequence_;
  Status status_;
  std::string saved_key_;
  bool valid_ = false;
};

}  // namespace

std::unique_ptr<Iterator> NewDBIterator(const Comparator* user_comparator,
                                        std::unique_ptr<Iterator> internal_iter,
                                        SequenceNumber snapshot,
                                        std::shared_ptr<const void> pin) {
  return std::make_unique<DBIter>(user_comparator, std::move(internal_iter),
                                  snapshot, std::move(pin));
}

}  // namespace lsmkv
