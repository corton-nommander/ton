/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include "td/utils/Time.h"
#include "td/utils/check.h"
#include "ton/ton-types.h"

namespace ton::validator {

inline std::optional<bool> parse_validated_state_handoff_flag(std::string_view value) {
  if (value == "0") {
    return false;
  }
  if (value == "1") {
    return true;
  }
  return std::nullopt;
}

inline bool validated_state_handoff_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("TON_NATIVE_VALIDATED_STATE_HANDOFF");
    if (value == nullptr) {
      return false;
    }
    auto parsed = parse_validated_state_handoff_flag(value);
    CHECK(parsed.has_value());
    return *parsed;
  }();
  return enabled;
}

inline bool validated_state_handoff_identity_matches(const BlockIdExt& cache_key, const BlockIdExt& state_block_id,
                                                     const RootHash& state_root_hash,
                                                     const RootHash& validated_state_root_hash) {
  return cache_key == state_block_id && state_root_hash == validated_state_root_hash;
}

inline bool validated_state_handoff_block_matches(const BlockIdExt& handle_id, const BlockIdExt& data_id,
                                                  const RootHash& actual_block_root_hash) {
  return handle_id == data_id && actual_block_root_hash == data_id.root_hash;
}

struct ValidatedStateHandoffCacheStats {
  td::uint64 hits{0};
  td::uint64 misses{0};
  td::uint64 inserts{0};
  td::uint64 evictions{0};
  td::uint64 expirations{0};
};

// A successful validation result is speculative until consensus accepts the
// exact BlockIdExt. Keep it out of the readable block-state cache and consume
// it on the first matching acceptance.
template <class Value>
class ValidatedStateHandoffCache {
 public:
  ValidatedStateHandoffCache(size_t max_entries, double ttl_seconds)
      : max_entries_(max_entries), ttl_seconds_(ttl_seconds) {
    CHECK(max_entries_ > 0);
    CHECK(ttl_seconds_ > 0.0);
  }

  void insert(BlockIdExt block_id, Value value, td::Timestamp now = td::Timestamp::now_cached()) {
    expire(now);
    ++stats_.inserts;

    Entry entry{std::move(value), td::Timestamp::in(ttl_seconds_, now), ++next_generation_};
    auto it = entries_.find(block_id);
    if (it != entries_.end()) {
      it->second = std::move(entry);
      return;
    }
    entries_.emplace(std::move(block_id), std::move(entry));
    if (entries_.size() <= max_entries_) {
      return;
    }

    auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto& left, const auto& right) {
      return left.second.generation < right.second.generation;
    });
    CHECK(oldest != entries_.end());
    entries_.erase(oldest);
    ++stats_.evictions;
  }

  std::optional<Value> take(const BlockIdExt& block_id, td::Timestamp now = td::Timestamp::now_cached()) {
    expire(now);
    auto it = entries_.find(block_id);
    if (it == entries_.end()) {
      ++stats_.misses;
      return std::nullopt;
    }
    ++stats_.hits;
    Value value = std::move(it->second.value);
    entries_.erase(it);
    return value;
  }

  void expire(td::Timestamp now = td::Timestamp::now_cached()) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.expires_at.is_in_past(now)) {
        it = entries_.erase(it);
        ++stats_.expirations;
      } else {
        ++it;
      }
    }
  }

  size_t size() const {
    return entries_.size();
  }

  const ValidatedStateHandoffCacheStats& stats() const {
    return stats_;
  }

 private:
  struct Entry {
    Value value;
    td::Timestamp expires_at;
    td::uint64 generation;
  };

  size_t max_entries_;
  double ttl_seconds_;
  td::uint64 next_generation_{0};
  std::map<BlockIdExt, Entry> entries_;
  ValidatedStateHandoffCacheStats stats_;
};

}  // namespace ton::validator
