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

// Bound the configured sealing reserve without allowing it to consume more
// than half of the local work budget.  Even the minimum 2s outer timeout then
// retains 500ms for finding and processing the first native fragment.
inline constexpr std::chrono::milliseconds max_tps_candidate_finalize_reserve_default{1'000};
inline constexpr std::chrono::milliseconds max_tps_candidate_finalize_reserve_min{100};
inline constexpr std::chrono::milliseconds max_tps_candidate_finalize_reserve_max{5'000};
// A fragment which begins immediately before the intake boundary still needs
// time to execute and reach the rollback/checkpoint decision.  The observed
// 512-transfer fragment cost is well below this conservative start guard.
inline constexpr std::chrono::milliseconds max_tps_candidate_fragment_start_guard{100};

constexpr std::chrono::milliseconds bound_max_tps_candidate_finalize_reserve(
    std::chrono::milliseconds work_budget, std::chrono::milliseconds requested) {
  if (work_budget <= std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  auto budget_cap = work_budget / 2;
  auto upper = std::min(max_tps_candidate_finalize_reserve_max, budget_cap);
  auto lower = std::min(max_tps_candidate_finalize_reserve_min, upper);
  return std::clamp(requested, lower, upper);
}

constexpr std::chrono::milliseconds max_tps_candidate_intake_timeout(
    std::chrono::milliseconds work_budget, std::chrono::milliseconds finalize_reserve) {
  auto reserve = bound_max_tps_candidate_finalize_reserve(work_budget, finalize_reserve);
  auto remaining = work_budget - reserve;
  auto start_guard = std::min(max_tps_candidate_fragment_start_guard, remaining / 4);
  return remaining - start_guard;
}

enum class NativeIntakeDeadlineAction { continue_work, idle, seal_committed, commit_first_fragment };

// Deadline policy is kept pure so the safety-critical boundary cases are
// compile-time tested independently from actor scheduling.
constexpr NativeIntakeDeadlineAction select_native_intake_deadline_action(bool deadline_reached,
                                                                           bool has_committed_fragment,
                                                                           bool has_staged_fragment) {
  if (!deadline_reached) {
    return NativeIntakeDeadlineAction::continue_work;
  }
  if (has_committed_fragment) {
    return NativeIntakeDeadlineAction::seal_committed;
  }
  if (has_staged_fragment) {
    return NativeIntakeDeadlineAction::commit_first_fragment;
  }
  return NativeIntakeDeadlineAction::idle;
}

constexpr bool should_extend_native_producer_wait(bool work_driven, bool producer_pending) {
  return !work_driven && producer_pending;
}

// Nominal interval targeted inside the local collation budget for installing
// final native state, building the block/state update and serializing the
// candidate.  A non-preemptible fragment may consume the separate start guard.
// Configurable with TON_SIMPLEX_MAX_TPS_FINALIZE_RESERVE_MS; defaults to 1s.
std::chrono::milliseconds max_tps_candidate_finalize_reserve();

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
