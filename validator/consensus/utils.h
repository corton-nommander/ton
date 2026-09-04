/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "common/errorcode.h"
#include "interfaces/block.h"
#include "interfaces/external-message.h"
#include "td/actor/common.h"
#include "td/actor/coro_task.h"
#include "td/utils/Status.h"
#include "ton/ton-types.h"

namespace ton::validator::consensus {

td::Result<double> get_candidate_gen_utime_exact(const BlockCandidate& candidate);

// Returns raw external-message hashes reconstructed from a compact native
// batch carried by the candidate. The result is sorted and deduplicated so it
// can be merged directly into a speculative-branch exclusion vector. A direct
// source-signed run contributes its parent NTRN hash once, not a synthetic
// hash for every flattened child.
td::Result<std::vector<Bits256>> get_candidate_native_external_hashes(const BlockCandidate& candidate);

// Returns every native source/nonce interval carried by a candidate. A
// direct-run batch keeps one atomic interval identified by its parent NTRN
// hash, while scalar NTFX metadata keeps logical_count == 1.
td::Result<std::vector<TrackedNativeExternalMessage>> get_candidate_native_external_messages(
    const BlockCandidate& candidate);

// Projects already-decoded candidate metadata to exclusive source nonce
// boundaries. Zero-length intervals and intervals whose exclusive end cannot
// be represented are rejected. The result is sorted and unique by
// (workchain, source), with the greatest boundary retained for duplicates.
td::Result<NativeSourceNonceFloors> get_native_source_nonce_floors(
    const std::vector<TrackedNativeExternalMessage>& messages);

// Deterministically folds `added` into `target`, retaining the greatest
// exclusive boundary for each source. `target` must already be sorted and
// unique; `added` may be unsorted or contain duplicate source records. The
// merge is linear in target size after normalizing only `added`.
void merge_native_source_nonce_floors(NativeSourceNonceFloors& target, const NativeSourceNonceFloors& added);

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

// Experimental checkpoint retention is fail-closed and independent from the
// broader max-TPS switch: only the literal value "1" enables it.  The caller
// still gates the policy to work-driven shardchain collation and every
// validator in a private deployment must opt in consistently.
constexpr bool parse_native_checkpoint_retain_ingress(std::string_view value) {
  return value == "1";
}

inline bool native_checkpoint_retain_ingress_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("TON_NATIVE_CHECKPOINT_RETAIN_INGRESS");
    return parse_native_checkpoint_retain_ingress(value ? std::string_view{value} : std::string_view{});
  }();
  return enabled;
}

// Dense post-commit packing is an explicit sidechain throughput tradeoff.  A
// committed checkpoint may wait up to 20 ms for more work when every validator
// opts in; the default remains 10 ms.  This does not change the independent
// partial-fragment refill or fixed checkpoint-latency deadlines.
inline constexpr double native_post_commit_pack_grace_default_seconds = 0.010;
inline constexpr double native_post_commit_pack_grace_throughput_seconds = 0.020;

constexpr bool parse_native_post_commit_pack_grace_20ms(std::string_view value) {
  return value == "1";
}

constexpr double select_native_post_commit_pack_grace_seconds(bool work_driven, bool throughput_opt_in) {
  return work_driven && throughput_opt_in ? native_post_commit_pack_grace_throughput_seconds
                                          : native_post_commit_pack_grace_default_seconds;
}

inline double native_post_commit_pack_grace_seconds(bool work_driven) {
  static const bool throughput_opt_in = [] {
    const char* value = std::getenv("TON_NATIVE_POST_COMMIT_PACK_GRACE_20MS");
    return parse_native_post_commit_pack_grace_20ms(value ? std::string_view{value} : std::string_view{});
  }();
  return select_native_post_commit_pack_grace_seconds(work_driven, throughput_opt_in);
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

// A native callback feeds the Collator through a much smaller physical
// transport window than the candidate-level selection allowance above. Keep
// the default at the existing two 512-message fragments, but permit a bounded
// opt-in prefill for throughput experiments. The window is deliberately a
// whole number of scheduler fragments: it preserves the native scheduler's
// fairness and makes the producer's one-fragment look-ahead explicit.
inline constexpr std::size_t native_ext_msg_transport_fragment_capacity = 512;
inline constexpr std::size_t native_ext_msg_transport_default_capacity =
    2 * native_ext_msg_transport_fragment_capacity;
inline constexpr std::size_t native_ext_msg_transport_max_capacity =
    16 * native_ext_msg_transport_fragment_capacity;

// Native transfer execution remains deliberately fair and cancellable in
// 512-message fragments.  The much more expensive exact ShardAccounts
// proof/storage-stat checkpoint can safely cover a short run of those
// fragments, provided no live candidate state is mutated before its single
// hard-limit preflight.  These are hard local bounds, not protocol limits:
// they cap both the uncommitted transfer journal and the time spent between
// exact checkpoints.
inline constexpr std::size_t native_checkpoint_coalesce_max_fragments = 4;
inline constexpr std::size_t native_checkpoint_coalesce_max_entries =
    native_checkpoint_coalesce_max_fragments * native_ext_msg_transport_fragment_capacity;
// A transfer can touch two accounts.  Flush before a four-fragment checkpoint
// reaches its worst-case fanout so high-cardinality traffic retains headroom
// for exact proof construction and the final candidate state install.
inline constexpr std::size_t native_checkpoint_coalesce_fanout_limit =
    6 * native_ext_msg_transport_fragment_capacity;
inline constexpr double native_checkpoint_coalesce_max_latency_seconds = 0.025;

// The checkpoint policy is pure so its safety boundaries remain independently
// testable. `ingress_boundary` means the bounded refill wait actually ended
// without another fragment (or the current source snapshot was partial), not
// a transient empty nonblocking queue probe. `headroom_limited` is raised by
// the conservative deferred-size reservation before another transfer is
// staged.
constexpr bool should_flush_native_checkpoint(std::size_t staged_entries, std::size_t staged_fragments,
                                              std::size_t staged_dirty_accounts, bool deadline_reached,
                                              bool ingress_boundary, bool headroom_limited,
                                              bool latency_expired) {
  return deadline_reached || ingress_boundary || headroom_limited || latency_expired ||
         staged_entries >= native_checkpoint_coalesce_max_entries ||
         staged_fragments >= native_checkpoint_coalesce_max_fragments ||
         staged_dirty_accounts >= native_checkpoint_coalesce_fanout_limit;
}

struct NativeCheckpointIngressRetentionState {
  bool enabled{false};
  bool work_driven{false};
  bool has_committed_fragment{false};
  bool has_pending_checkpoint{false};
  bool ingress_boundary{false};
  bool bounded_refill_timed_out{false};
  bool latency_window_open{false};
  bool intake_deadline_reached{false};
  bool checkpoint_deadline_reached{false};
  bool headroom_limited{false};
  bool capacity_reached{false};
  bool fanout_reached{false};
  bool protocol_capacity_reached{false};
  bool protocol_capacity_deferred{false};
};

// The opt-in treatment masks ingress only when it is the sole reason to flush
// an already-bounded checkpoint.  It never weakens the first exact rollback
// anchor, intake/finalization deadline, size headroom, protocol capacity,
// coalescing capacity, fanout, or fixed latency boundaries.
constexpr bool should_retain_native_checkpoint_at_ingress(const NativeCheckpointIngressRetentionState& state) {
  return state.enabled && state.work_driven && state.has_committed_fragment && state.has_pending_checkpoint &&
         state.ingress_boundary && state.bounded_refill_timed_out && state.latency_window_open &&
         !state.intake_deadline_reached && !state.checkpoint_deadline_reached && !state.headroom_limited &&
         !state.capacity_reached && !state.fanout_reached && !state.protocol_capacity_reached &&
         !state.protocol_capacity_deferred;
}

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

constexpr std::size_t parse_native_ext_msg_transport_capacity(std::string_view value) {
  if (value.empty()) {
    return native_ext_msg_transport_default_capacity;
  }
  std::size_t parsed = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') {
      return native_ext_msg_transport_default_capacity;
    }
    auto digit = static_cast<std::size_t>(ch - '0');
    if (parsed < native_ext_msg_transport_max_capacity) {
      if (parsed > (native_ext_msg_transport_max_capacity - digit) / 10) {
        parsed = native_ext_msg_transport_max_capacity;
      } else {
        parsed = parsed * 10 + digit;
      }
    }
  }
  if (parsed < native_ext_msg_transport_fragment_capacity ||
      parsed % native_ext_msg_transport_fragment_capacity != 0) {
    return native_ext_msg_transport_default_capacity;
  }
  return std::min(parsed, native_ext_msg_transport_max_capacity);
}

// Non-native callbacks retain their ordinary queue capacity. A native
// transport window is never allowed to exceed the per-candidate allowance,
// including intentionally small queue-limit experiments.
constexpr std::size_t select_native_ext_msg_transport_capacity(bool native_streaming, std::size_t queue_capacity,
                                                                std::string_view configured_window) {
  return native_streaming ? std::min(queue_capacity, parse_native_ext_msg_transport_capacity(configured_window))
                          : queue_capacity;
}

// Native candidates are serialized as indexed mode-31 BOCs.  The generic
// BlockLimitStatus estimate intentionally stays cheap and does not account
// exactly for the index, internal hashes, and the structures created while
// the final state update and block envelope are assembled.  Reserve one
// seventh of the consensus wire-size limit before accepting another native
// state checkpoint.  At the sidechain's 10 MiB limit this is 1,497,965 bytes,
// covering the largest 1,396,041-byte estimator gap observed in the saturated
// desktop runs with 101,924 bytes left over.  The estimator retains 85.7% of
// the configured budget; measured mode-31 expansion turns that into higher
// wire utilization instead of wasting the full reserve.
inline constexpr td::uint64 native_candidate_size_reserve_divisor = 7;

constexpr td::uint64 native_candidate_size_reserve(td::uint64 consensus_max_block_size) {
  return consensus_max_block_size / native_candidate_size_reserve_divisor;
}

constexpr td::uint64 native_candidate_estimate_budget(td::uint64 consensus_max_block_size) {
  return consensus_max_block_size - native_candidate_size_reserve(consensus_max_block_size);
}

constexpr bool native_candidate_estimate_fits(td::uint64 estimated_bytes, td::uint64 consensus_max_block_size) {
  // Keep the boundary itself reserved: the final structures added after the
  // native checkpoint are non-empty even for the smallest useful candidate.
  return estimated_bytes < native_candidate_estimate_budget(consensus_max_block_size);
}

constexpr bool candidate_serialized_size_fits(td::uint64 serialized_bytes, td::uint64 consensus_max_block_size) {
  return serialized_bytes <= consensus_max_block_size;
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

constexpr std::chrono::milliseconds bound_max_tps_candidate_finalize_reserve(std::chrono::milliseconds work_budget,
                                                                             std::chrono::milliseconds requested) {
  if (work_budget <= std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  auto budget_cap = work_budget / 2;
  auto upper = std::min(max_tps_candidate_finalize_reserve_max, budget_cap);
  auto lower = std::min(max_tps_candidate_finalize_reserve_min, upper);
  return std::clamp(requested, lower, upper);
}

constexpr std::chrono::milliseconds max_tps_candidate_intake_timeout(std::chrono::milliseconds work_budget,
                                                                     std::chrono::milliseconds finalize_reserve) {
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

enum class NativeCheckpointSelectionBoundaryAction {
  continue_selection,
  flush_before_selection,
  seal_selected_fragment,
};

// Once a checkpoint's fixed latency bound expires, do not pull another
// fragment into it. Flush immediately when selection has not started; if a
// fragment is already selected, seal that selection boundary so the caller's
// pre-execution guard can flush the old checkpoint first.
constexpr NativeCheckpointSelectionBoundaryAction select_native_checkpoint_selection_boundary_action(
    bool has_pending_checkpoint, bool latency_expired, bool selected_fragment_empty) {
  if (!has_pending_checkpoint || !latency_expired) {
    return NativeCheckpointSelectionBoundaryAction::continue_selection;
  }
  return selected_fragment_empty ? NativeCheckpointSelectionBoundaryAction::flush_before_selection
                                 : NativeCheckpointSelectionBoundaryAction::seal_selected_fragment;
}

enum class NativeCheckpointRefillBoundaryAction { retain, flush, seal_committed, commit_first_fragment };

// The collator only asks this after a queue probe or bounded refill returns no
// messages. A successful refill (including a marker-only wake, which is
// retried against the same fixed deadline) keeps the journaled checkpoint.
constexpr NativeCheckpointRefillBoundaryAction select_native_checkpoint_refill_boundary_action(
    bool has_pending_checkpoint, bool refill_returned_messages, bool intake_deadline_reached,
    bool has_committed_fragment) {
  if (!has_pending_checkpoint || refill_returned_messages) {
    return NativeCheckpointRefillBoundaryAction::retain;
  }
  switch (select_native_intake_deadline_action(intake_deadline_reached, has_committed_fragment, true)) {
    case NativeIntakeDeadlineAction::continue_work:
    case NativeIntakeDeadlineAction::idle:
      return NativeCheckpointRefillBoundaryAction::flush;
    case NativeIntakeDeadlineAction::seal_committed:
      return NativeCheckpointRefillBoundaryAction::seal_committed;
    case NativeIntakeDeadlineAction::commit_first_fragment:
      return NativeCheckpointRefillBoundaryAction::commit_first_fragment;
  }
  return NativeCheckpointRefillBoundaryAction::flush;
}

enum class NativeQueueRefillAction { stop, wait_first_work, wait_fragment, wait_post_commit_idle };

struct NativeQueueRefillState {
  bool work_driven{false};
  bool cancelled{false};
  bool intake_deadline_reached{false};
  bool fragment_full{false};
  bool has_staged_fragment{false};
  bool has_committed_fragment{false};
  bool first_work_window_open{false};
  bool fragment_window_open{false};
  bool post_commit_idle_window_open{false};
  bool producer_pending{false};
};

// Work-driven native collation owns its queue waits inside one invocation.
// In particular, producer progress never extends a fixed fragment or
// post-commit idle window.  A marker-only pop leaves this state unchanged, so
// the caller waits again until the original window expires or work arrives.
constexpr NativeQueueRefillAction select_native_queue_refill_action(const NativeQueueRefillState& state) {
  if (!state.work_driven || state.cancelled || state.intake_deadline_reached || state.fragment_full) {
    return NativeQueueRefillAction::stop;
  }
  if (state.has_staged_fragment) {
    return state.fragment_window_open ? NativeQueueRefillAction::wait_fragment : NativeQueueRefillAction::stop;
  }
  if (state.has_committed_fragment) {
    return state.post_commit_idle_window_open ? NativeQueueRefillAction::wait_post_commit_idle
                                              : NativeQueueRefillAction::stop;
  }
  if (state.first_work_window_open || state.producer_pending) {
    return NativeQueueRefillAction::wait_first_work;
  }
  return NativeQueueRefillAction::stop;
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
