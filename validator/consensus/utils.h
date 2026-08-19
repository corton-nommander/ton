/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "common/errorcode.h"
#include "interfaces/block.h"
#include "interfaces/external-message.h"
#include "td/actor/common.h"
#include "td/actor/coro_task.h"
#include "td/utils/Status.h"
#include "ton/ton-types.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace ton::validator::consensus {

td::Result<double> get_candidate_gen_utime_exact(const BlockCandidate& candidate);

// Returns raw external-message hashes reconstructed from a compact native
// batch carried by the candidate. The result is sorted and deduplicated so it
// can be merged directly into a speculative-branch exclusion vector.
td::Result<std::vector<Bits256>> get_candidate_native_external_hashes(const BlockCandidate& candidate);

td::Result<std::vector<FinalizedNativeExternalMessage>> get_candidate_native_external_messages(
    const BlockCandidate& candidate);

// Explicit sidechain-only throughput mode.  The environment switch is kept
// outside consensus configuration deliberately: every validator in the
// private deployment must opt in, while public/default TON behaviour remains
// unchanged.
inline bool max_tps_mode_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("TON_SIMPLEX_MAX_TPS");
    if (!value) {
      return false;
    }
    std::string_view parsed{value};
    return parsed == "1" || parsed == "true" || parsed == "TRUE" || parsed == "yes" || parsed == "YES";
  }();
  return enabled;
}

// Max-TPS changes candidate scheduling only for shardchain production.  The
// masterchain continues to use its normal target-rate pacing, minimum block
// interval, and failure/skip deadlines even when the process also produces a
// work-driven sidechain.  Keeping this predicate in one place prevents the two
// consensus actors from silently choosing different timing policies.
constexpr bool select_work_driven_max_tps_mode(bool max_tps_mode, bool is_masterchain) {
  return max_tps_mode && !is_masterchain;
}

inline bool work_driven_max_tps_mode_enabled(ShardIdFull shard) {
  return select_work_driven_max_tps_mode(max_tps_mode_enabled(), shard.is_masterchain());
}

// The native pool and collator intentionally share TON_NATIVE_COLLATOR_QUEUE_LIMIT.
// A full protocol batch is enough to keep one candidate busy; accepting more
// queue entries would only retain extra message references which cannot fit in
// that candidate.  The default matches ExtMessagePool's default snapshot size.
inline constexpr std::size_t native_collator_queue_default_capacity = 32'768;
inline constexpr std::size_t native_collator_queue_max_capacity = 65'536;
inline constexpr std::size_t standard_collator_queue_capacity = 500;

constexpr std::size_t parse_native_collator_queue_capacity(std::string_view value) {
  if (value.empty()) {
    return native_collator_queue_default_capacity;
  }
  std::size_t parsed = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') {
      return native_collator_queue_default_capacity;
    }
    auto digit = static_cast<std::size_t>(ch - '0');
    if (parsed < native_collator_queue_max_capacity) {
      if (parsed > (native_collator_queue_max_capacity - digit) / 10) {
        parsed = native_collator_queue_max_capacity;
      } else {
        parsed = parsed * 10 + digit;
      }
    }
  }
  if (!parsed) {
    return native_collator_queue_default_capacity;
  }
  return std::min(parsed, native_collator_queue_max_capacity);
}

constexpr std::size_t select_collator_queue_capacity(bool max_tps_mode, std::string_view native_limit) {
  return max_tps_mode ? parse_native_collator_queue_capacity(native_limit) : standard_collator_queue_capacity;
}

// Failure budget for one work-driven candidate. It does not delay a
// successful block: Simplex only consults it when deciding that local
// production has failed and the remaining leader window must be skipped.
// Values are clamped to [2s, 60s]; the sidechain default is 5s.
std::chrono::milliseconds max_tps_candidate_timeout();

// Portion of the outer failure budget available to local collation and
// candidate construction. The remainder is reserved for dissemination,
// validation, votes, and notarization.
std::chrono::milliseconds max_tps_candidate_work_timeout();

// Expected control result for a work-driven native collator whose ingress
// queue stayed empty for the whole local work window.  It is deliberately
// distinct from generic notready failures so BlockProducer can leave the
// leader window idle without creating an empty candidate or retry-spinning.
inline td::Status native_collation_idle_status() {
  return td::Status::Error(ErrorCode::notready, "native ingress idle for candidate work window");
}

inline bool is_native_collation_idle_status(const td::Status& status) {
  return status.is_error() && status.code() == ErrorCode::notready &&
         status.message() == "native ingress idle for candidate work window";
}

}  // namespace ton::validator::consensus
