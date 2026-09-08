#pragma once

#include <optional>
#include <utility>
#include "ton/ton-types.h"

namespace ton::validator {

// Actor-local, single-entry cache of immutable configuration facts. Neither a
// sequence number nor the union of shard tips identifies an exact state.
template <class Value>
class NativeAdmissionConfigCache {
 public:
  const Value* lookup(const BlockIdExt& block, const RootHash& root) const {
    return entry_ && entry_->block == block && entry_->root == root ? &entry_->value : nullptr;
  }
  void store(BlockIdExt block, RootHash root, Value value) {
    entry_.emplace(Entry{std::move(block), std::move(root), std::move(value)});
  }
  void clear() { entry_.reset(); }

 private:
  struct Entry {
    BlockIdExt block;
    RootHash root;
    Value value;
  };
  std::optional<Entry> entry_;
};

}  // namespace ton::validator
