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

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "crypto/common/refcnt.hpp"
#include "crypto/vm/cells.h"
#include "ton/ton-types.h"

namespace ton {

namespace validator {

class ExtMessage : public td::CntObject {
 public:
  using Hash = Bits256;

  virtual ~ExtMessage() = default;
  virtual AccountIdPrefixFull shard() const = 0;
  virtual td::BufferSlice serialize() const = 0;
  virtual td::Ref<vm::Cell> root_cell() const = 0;
  virtual Hash hash() const = 0;
  virtual Hash hash_norm() const = 0;
  virtual ton::WorkchainId wc() const = 0;
  virtual ton::StdSmcAddress addr() const = 0;
};

// Canonical identity of a compact native external consumed by a finalized
// block.  Source/nonce accompany the raw hash so a validator can purge every
// locally admitted losing variant of that now-obsolete nonce, even if it never
// held the winning byte representation in its mempool.
struct FinalizedNativeExternalMessage {
  ExtMessage::Hash hash;
  WorkchainId workchain{basechainId};
  StdSmcAddress source;
  td::uint64 nonce{0};
};

// Ordered result of admitting one element of a sendMessageBatch request.  This
// intentionally contains no actor-local state: ValidatorManager consumes the
// pool's broadcast handles and returns only these stable per-input statuses to
// the liteserver.
struct ExternalMessageAdmissionResult {
  bool accepted{false};
  int error_code{0};
  std::string error_message;

  static ExternalMessageAdmissionResult success() {
    return {.accepted = true, .error_code = 0, .error_message = {}};
  }
  static ExternalMessageAdmissionResult failure(td::Status error) {
    return {.accepted = false,
            .error_code = error.code(),
            .error_message = error.message().str()};
  }
};

using ExternalMessageAdmissionResults = std::vector<ExternalMessageAdmissionResult>;

struct NativeAdmissionOrderKey {
  WorkchainId workchain{basechainId};
  StdSmcAddress source;
  td::uint64 nonce{0};
  Bits256 hash;
  std::size_t input_index{0};

  bool operator<(const NativeAdmissionOrderKey &other) const {
    if (workchain != other.workchain) {
      return workchain < other.workchain;
    }
    if (source != other.source) {
      return source < other.source;
    }
    if (nonce != other.nonce) {
      return nonce < other.nonce;
    }
    if (hash != other.hash) {
      return hash < other.hash;
    }
    return input_index < other.input_index;
  }
};

inline std::vector<std::size_t> order_native_admissions(std::vector<NativeAdmissionOrderKey> keys) {
  std::sort(keys.begin(), keys.end());
  std::vector<std::size_t> order;
  order.reserve(keys.size());
  for (const auto &key : keys) {
    order.push_back(key.input_index);
  }
  return order;
}

}  // namespace validator

}  // namespace ton
