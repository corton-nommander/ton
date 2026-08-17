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

#include <cstdlib>
#include <chrono>
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
// unchanged.  target_rate still controls Simplex failure/skip timeouts, but it
// no longer paces successful candidate production in this mode.
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
