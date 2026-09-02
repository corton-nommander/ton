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

#include <cstddef>

#include "common/bitstring.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "td/utils/buffer.h"

namespace ton::validator {

// Keep the native-only probe and the cache admission queue bounded before the
// message reaches the normal external-message size/depth validation path.
inline constexpr std::size_t native_send_message_coalescing_max_message_bytes = 4 << 10;

class LiteServerCache : public td::actor::Actor {
 public:
  ~LiteServerCache() override = default;

  virtual void lookup(td::Bits256 key, td::Promise<td::BufferSlice> promise) = 0;
  virtual void update(td::Bits256 key, td::BufferSlice value) = 0;

  // The cache only owns in-flight sendMessage queries. The caller-provided
  // token makes cleanup idempotent and prevents a rejected duplicate from
  // erasing the original query's entry.
  virtual void process_send_message(td::Bits256 key, td::uint64 owner, td::Promise<td::Unit> promise) = 0;
  virtual void drop_send_message_from_cache(td::Bits256 key, td::uint64 owner) = 0;

  // Individual native transfers can share the manager's bounded batch
  // admission path. The deadline belongs to this request, rather than to the
  // coalescing window: implementations must never admit it after this point.
  // Non-native sendMessage requests retain their historical direct path.
  virtual void process_native_send_message(td::BufferSlice data, td::Timestamp deadline,
                                           td::Promise<td::Unit> promise) = 0;
};

}  // namespace ton::validator
