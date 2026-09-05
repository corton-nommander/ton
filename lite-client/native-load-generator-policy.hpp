#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <string_view>
#include <type_traits>
#include <vector>

namespace native_load {

struct AdaptiveCwndAckResult {
  double cwnd{1.0};
  bool limited{false};
};

struct AdaptiveCwndLossResult {
  double cwnd{1.0};
  bool minimum_limited{false};
};

// AIMD windows count messages, not wire queries. A zero configured limit keeps
// the historical hard-limit-only behavior; otherwise every ACK grows toward
// the independently distributed admission ceiling without changing decrease
// semantics.
inline AdaptiveCwndAckResult adaptive_cwnd_after_ack(double current, double hard_limit, double configured_limit) {
  auto limit = configured_limit > 0.0 ? std::min(hard_limit, configured_limit) : hard_limit;
  auto wanted = current + 1.0 / std::max(1.0, current);
  return {.cwnd = std::min(limit, wanted), .limited = wanted > limit};
}

struct AdaptiveCwndLogicalAckResult {
  double cwnd{1.0};
  std::uint32_t limited_acks{0};
};

// Transport grouping must not change AIMD growth: acknowledge every logical
// output, including finite repair tails, exactly as individual submissions do.
inline AdaptiveCwndLogicalAckResult adaptive_cwnd_after_logical_acks(
    double current, double hard_limit, double configured_limit,
    std::uint32_t logical_count) {
  AdaptiveCwndLogicalAckResult result{current, 0};
  for (std::uint32_t i = 0; i < logical_count; ++i) {
    auto next = adaptive_cwnd_after_ack(result.cwnd, hard_limit, configured_limit);
    result.cwnd = next.cwnd;
    result.limited_acks += next.limited ? 1u : 0u;
  }
  return result;
}

// Keep the loss response usable by indivisible logical-message quanta.  The
// caller bounds minimum_dispatch_window by that client's configured ceiling;
// scalar admission therefore retains the historical floor of one.
inline AdaptiveCwndLossResult adaptive_cwnd_after_loss(
    double current, double minimum_dispatch_window) {
  auto minimum = std::max(1.0, minimum_dispatch_window);
  auto halved = current * 0.5;
  return {.cwnd = std::max(minimum, halved),
          .minimum_limited = halved < minimum};
}

inline std::uint32_t distributed_share(std::uint32_t total, std::uint32_t index, std::uint32_t count) {
  if (count == 0 || index >= count) {
    return 0;
  }
  return total / count + (index < total % count ? 1u : 0u);
}

inline bool valid_adaptive_max_cwnd(std::uint32_t configured_limit, std::uint32_t connections,
                                    std::uint32_t max_inflight) {
  return configured_limit == 0 || (configured_limit >= connections && configured_limit <= max_inflight);
}

// A submission query can carry many messages, so admission-query credit is
// deliberately independent from the message-count AIMD and hard-inflight
// ceilings. Zero keeps the historical unlimited-per-client query behavior.
constexpr std::uint32_t max_submit_queries_per_client = 65536;

inline bool valid_submit_max_queries_per_client(std::uint32_t configured_limit) {
  return configured_limit <= max_submit_queries_per_client;
}

inline bool admission_query_credit_available(std::uint32_t configured_limit,
                                             std::uint32_t queries_inflight) {
  return configured_limit == 0 || queries_inflight < configured_limit;
}

inline bool admission_query_credit_at_cap(std::uint32_t configured_limit,
                                          std::uint32_t queries_inflight) {
  return configured_limit != 0 && queries_inflight >= configured_limit;
}

inline bool client_can_dispatch_admission_query(std::uint32_t message_capacity,
                                                std::uint32_t configured_query_limit,
                                                std::uint32_t queries_inflight) {
  return message_capacity != 0 &&
         admission_query_credit_available(configured_query_limit, queries_inflight);
}

inline bool acquire_admission_query_credit(std::uint32_t configured_limit,
                                           std::uint32_t& queries_inflight) {
  if (!admission_query_credit_available(configured_limit, queries_inflight) ||
      queries_inflight == std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  ++queries_inflight;
  return true;
}

inline bool release_admission_query_credit(std::uint32_t& queries_inflight) {
  if (queries_inflight == 0) {
    return false;
  }
  --queries_inflight;
  return true;
}

// The caller lower-cases the server diagnostic before classification.  Keep
// this deliberately narrow: ErrorCode::notready is also used for real
// admission pressure and revision races, while these exact diagnostics mean
// that the account snapshot used by admission trails a newer canonical account
// state already observed by the native-transfer pool. Keep the legacy marker
// during rolling upgrades so a rebuilt generator can drive an older node.
inline bool is_canonical_state_lag_diagnostic(std::string_view lowercase_diagnostic) {
  constexpr std::string_view legacy_marker = "canonical native account state has not caught up with finalized balance";
  constexpr std::string_view canonical_watermark_marker =
      "native account state predates the latest observed canonical state";
  return lowercase_diagnostic.find(legacy_marker) != std::string_view::npos ||
         lowercase_diagnostic.find(canonical_watermark_marker) != std::string_view::npos;
}

// These exact diagnostics describe an NTRN admission snapshot or account
// revision race after work has already been verified. They are recoverable,
// but they are not generic admission pressure and therefore must not make the
// signed-run generator halve its AIMD window. Keep the scalar path unchanged:
// the same native-account revision checks can be reached by a legacy NTFX,
// but only a concrete NTRN task receives this narrow exemption. The
// caller lower-cases the server diagnostic before classification.
inline bool is_native_signed_run_admission_snapshot_or_revision_race_diagnostic(
    bool native_signed_run_task, std::string_view lowercase_diagnostic) {
  if (!native_signed_run_task) {
    return false;
  }
  constexpr std::string_view snapshot_changed_marker =
      "native transfer run admission snapshot changed; retry";
  constexpr std::string_view signature_revision_marker =
      "native account changed during signature verification; retry admission";
  constexpr std::string_view insertion_revision_marker =
      "native account changed before mempool insertion; retry admission";
  constexpr std::string_view commit_revision_marker =
      "native account changed before mempool commit; retry admission";
  return lowercase_diagnostic.find(snapshot_changed_marker) != std::string_view::npos ||
         lowercase_diagnostic.find(signature_revision_marker) != std::string_view::npos ||
         lowercase_diagnostic.find(insertion_revision_marker) != std::string_view::npos ||
         lowercase_diagnostic.find(commit_revision_marker) != std::string_view::npos;
}

// Preserve the existing congestion response for timeouts, explicit pressure,
// and every unclassified not-ready response. Snapshot/revision races retain
// their existing retry path; this decision only avoids treating that narrow
// NTRN race as a congestion signal.
inline bool should_decrease_adaptive_cwnd_for_admission_failure(
    bool timeout, bool full, bool rate_limit, bool not_ready, bool canonical_state_lag,
    bool native_signed_run_snapshot_or_revision_race) {
  return timeout || full || rate_limit ||
         (not_ready && !canonical_state_lag && !native_signed_run_snapshot_or_revision_race);
}

inline double canonical_state_lag_retry_delay_seconds(std::uint32_t consecutive_failures,
                                                      std::uint32_t initial_backoff_ms,
                                                      std::uint32_t maximum_backoff_ms) {
  auto delay = static_cast<double>(initial_backoff_ms) / 1000.0;
  auto maximum = static_cast<double>(maximum_backoff_ms) / 1000.0;
  for (std::uint32_t i = 1; i < consecutive_failures && delay < maximum; ++i) {
    delay *= 2.0;
  }
  return std::min(delay, maximum);
}

inline bool retry_horizon_elapsed(double first_retry_at, double now, double retry_horizon_seconds) {
  return first_retry_at >= 0.0 && now >= first_retry_at && now - first_retry_at >= retry_horizon_seconds;
}

inline double clamp_retry_delay_to_horizon(double requested_delay_seconds, double first_retry_at, double now,
                                           double retry_horizon_seconds) {
  if (first_retry_at < 0.0 || now < first_retry_at) {
    return std::max(0.0, requested_delay_seconds);
  }
  auto remaining = std::max(0.0, retry_horizon_seconds - (now - first_retry_at));
  return std::min(std::max(0.0, requested_delay_seconds), remaining);
}

class ReadySourceQueue {
 public:
  enum class PopKind { empty, stale, ready };

  struct PopResult {
    PopKind kind{PopKind::empty};
    std::size_t source_idx{0};
  };

  void reset(std::size_t source_count) {
    sources_.assign(source_count, SourceState{});
    entries_.clear();
  }

  bool enqueue(std::size_t source_idx) {
    if (source_idx >= sources_.size()) {
      return false;
    }
    auto& source = sources_[source_idx];
    if (source.disabled || source.queued) {
      return false;
    }
    source.queued = true;
    ++source.generation;
    entries_.push_back(Entry{source_idx, source.generation});
    return true;
  }

  void invalidate(std::size_t source_idx) {
    if (source_idx >= sources_.size()) {
      return;
    }
    auto& source = sources_[source_idx];
    source.queued = false;
    ++source.generation;
  }

  void disable(std::size_t source_idx) {
    if (source_idx >= sources_.size()) {
      return;
    }
    sources_[source_idx].disabled = true;
    invalidate(source_idx);
  }

  PopResult pop() {
    if (entries_.empty()) {
      return {};
    }
    auto entry = entries_.front();
    entries_.pop_front();
    if (entry.source_idx >= sources_.size()) {
      return {PopKind::stale, entry.source_idx};
    }
    auto& source = sources_[entry.source_idx];
    if (source.disabled || !source.queued || source.generation != entry.generation) {
      return {PopKind::stale, entry.source_idx};
    }
    source.queued = false;
    return {PopKind::ready, entry.source_idx};
  }

  bool empty() const {
    return entries_.empty();
  }

  std::size_t size() const {
    return entries_.size();
  }

 private:
  struct SourceState {
    bool queued{false};
    bool disabled{false};
    std::uint64_t generation{0};
  };

  struct Entry {
    std::size_t source_idx{0};
    std::uint64_t generation{0};
  };

  std::vector<SourceState> sources_;
  std::deque<Entry> entries_;
};

// Leading-edge batching gate for ordinary first submissions.  The first
// dispatchable source head opens a bounded window; later ready heads join that
// window without extending it.  Once the deadline is reached it stays open
// until the worker proves that no fresh source head remains, so client-capacity
// starvation cannot repeatedly re-arm the delay.
class SubmitCoalescer {
 public:
  enum class ReleaseReason { blocked, bypass, deadline, full_batch };

  void note_fresh_ready(double now, double delay_seconds) {
    if (!armed_) {
      armed_ = true;
      deadline_ = now + std::max(0.0, delay_seconds);
    }
  }

  void note_queue_empty() {
    armed_ = false;
    deadline_ = -1.0;
  }

  ReleaseReason release_reason(double now, std::size_t dispatchable_fresh, std::size_t full_batch_size,
                               bool bypass) const {
    if (bypass) {
      return ReleaseReason::bypass;
    }
    if (!armed_) {
      return ReleaseReason::blocked;
    }
    if (full_batch_size > 0 && dispatchable_fresh >= full_batch_size) {
      return ReleaseReason::full_batch;
    }
    return now >= deadline_ ? ReleaseReason::deadline : ReleaseReason::blocked;
  }

  bool armed() const {
    return armed_;
  }

  double deadline() const {
    return deadline_;
  }

 private:
  bool armed_{false};
  double deadline_{-1.0};
};

template <class Iterator, class IsReady>
std::size_t bounded_contiguous_ready_run(Iterator begin, Iterator end, std::size_t limit, IsReady&& is_ready) {
  if (begin == end || limit == 0) {
    return 0;
  }
  using Key = std::decay_t<decltype(begin->first)>;
  auto expected = begin->first;
  std::size_t count = 0;
  while (begin != end && count < limit && begin->first == expected && is_ready(begin->second)) {
    ++count;
    ++begin;
    if (expected == std::numeric_limits<Key>::max()) {
      break;
    }
    ++expected;
  }
  return count;
}

inline std::size_t active_tasks_after_source_quarantine(std::size_t active_tasks, std::size_t source_tasks) {
  return source_tasks <= active_tasks ? active_tasks - source_tasks : 0;
}

inline bool retry_entry_can_wake(bool task_is_active, bool task_is_waiting, double task_retry_at,
                                 double entry_retry_at) {
  return task_is_active && task_is_waiting && task_retry_at == entry_retry_at;
}

// Native-load reporting has two independent units once one NTRN parent can
// authorize several transfers: physical external BOC bodies and the logical
// transfers they carry.  Keep the conversion explicit so transport metrics
// never silently turn into logical-TPS metrics in signed-run mode.
struct PhysicalLogicalMessageCounts {
  std::uint64_t physical_messages{0};
  std::uint64_t logical_transfers{0};
};

constexpr PhysicalLogicalMessageCounts single_submission_message_counts(
    std::uint64_t logical_transfers) {
  return {logical_transfers == 0 ? std::uint64_t{0} : std::uint64_t{1}, logical_transfers};
}

constexpr PhysicalLogicalMessageCounts batch_submission_message_counts(
    std::uint64_t physical_messages, std::uint64_t logical_transfers) {
  return {physical_messages, logical_transfers};
}

constexpr PhysicalLogicalMessageCounts source_issue_burst_message_counts(
    bool source_signed_run, std::uint64_t logical_transfers) {
  if (logical_transfers == 0) {
    return {};
  }
  return {source_signed_run ? std::uint64_t{1} : logical_transfers, logical_transfers};
}

constexpr std::uint32_t canonical_lane_equal_share_bps = 10000;
constexpr std::uint32_t default_canonical_lane_tolerance_bps = 500;
constexpr std::uint32_t max_canonical_lane_depth = 60;

// TON-free policy evidence for canonical per-lane reporting. Counts are
// measured over the same proof-anchored whole-second cohort as the aggregate;
// this helper only checks topology, reconciliation, activity, and balance.
struct CanonicalLaneBalanceSummary {
  bool required{false};
  bool valid{false};
  bool depth_valid{true};
  bool tolerance_valid{true};
  bool topology_complete{false};
  bool totals_reconcile{false};
  bool every_lane_active{false};
  bool within_tolerance{false};
  bool measured_transfers_sum_overflow{false};
  std::uint32_t depth{0};
  std::uint32_t tolerance_bps{default_canonical_lane_tolerance_bps};
  std::uint32_t minimum_allowed_equal_share_bps{canonical_lane_equal_share_bps -
                                                 default_canonical_lane_tolerance_bps};
  std::uint32_t maximum_allowed_equal_share_bps{canonical_lane_equal_share_bps +
                                                 default_canonical_lane_tolerance_bps};
  std::uint64_t expected_lanes{0};
  std::uint64_t observed_lanes{0};
  std::uint64_t aggregate_measured_transfers{0};
  std::uint64_t measured_transfers_sum{0};
  std::uint64_t min_measured_transfers{0};
  std::uint64_t max_measured_transfers{0};
  std::uint64_t min_equal_share_bps{0};
  std::uint64_t max_equal_share_bps{0};
};

namespace detail {

// Compare a*b*c with d*e without allowing either product to wrap. The right
// side always fits uint128 because it has only two uint64 factors. If the left
// side exceeds uint128, it is therefore strictly greater than the right side.
inline int compare_three_factor_product(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                        std::uint64_t d, std::uint64_t e) {
  using Wide = unsigned __int128;
  constexpr auto wide_max = ~static_cast<Wide>(0);
  Wide left = a;
  if ((b != 0 && left > wide_max / b)) {
    return 1;
  }
  left *= b;
  if ((c != 0 && left > wide_max / c)) {
    return 1;
  }
  left *= c;
  auto right = static_cast<Wide>(d) * e;
  if (left < right) {
    return -1;
  }
  return left > right ? 1 : 0;
}

inline std::uint64_t equal_share_bps(std::uint64_t measured_transfers,
                                     std::uint64_t expected_lanes,
                                     std::uint64_t aggregate_measured_transfers) {
  if (aggregate_measured_transfers == 0) {
    return 0;
  }
  constexpr auto result_max = std::numeric_limits<std::uint64_t>::max();
  if (compare_three_factor_product(measured_transfers, expected_lanes,
                                   canonical_lane_equal_share_bps,
                                   aggregate_measured_transfers, result_max) > 0) {
    return result_max;
  }
  using Wide = unsigned __int128;
  auto numerator = static_cast<Wide>(measured_transfers) * expected_lanes *
                   canonical_lane_equal_share_bps;
  return static_cast<std::uint64_t>(numerator / aggregate_measured_transfers);
}

}  // namespace detail

inline CanonicalLaneBalanceSummary summarize_canonical_lane_balance(
    std::uint32_t depth, std::uint64_t aggregate_measured_transfers,
    const std::vector<std::uint64_t>& lane_measured_transfers,
    std::uint32_t tolerance_bps = default_canonical_lane_tolerance_bps) {
  CanonicalLaneBalanceSummary result;
  result.required = depth > 0;
  result.depth = depth;
  result.tolerance_bps = tolerance_bps;
  result.depth_valid = depth <= max_canonical_lane_depth;
  result.tolerance_valid = tolerance_bps <= canonical_lane_equal_share_bps;
  if (result.tolerance_valid) {
    result.minimum_allowed_equal_share_bps = canonical_lane_equal_share_bps - tolerance_bps;
    result.maximum_allowed_equal_share_bps = canonical_lane_equal_share_bps + tolerance_bps;
  }
  result.observed_lanes = static_cast<std::uint64_t>(lane_measured_transfers.size());
  result.aggregate_measured_transfers = aggregate_measured_transfers;

  if (!lane_measured_transfers.empty()) {
    result.min_measured_transfers = lane_measured_transfers.front();
    result.max_measured_transfers = lane_measured_transfers.front();
  }
  for (auto measured_transfers : lane_measured_transfers) {
    result.min_measured_transfers = std::min(result.min_measured_transfers, measured_transfers);
    result.max_measured_transfers = std::max(result.max_measured_transfers, measured_transfers);
    if (result.measured_transfers_sum_overflow ||
        result.measured_transfers_sum >
            std::numeric_limits<std::uint64_t>::max() - measured_transfers) {
      result.measured_transfers_sum_overflow = true;
      result.measured_transfers_sum = std::numeric_limits<std::uint64_t>::max();
    } else {
      result.measured_transfers_sum += measured_transfers;
    }
  }
  result.totals_reconcile = !result.measured_transfers_sum_overflow &&
                            result.measured_transfers_sum == aggregate_measured_transfers;

  // Scalar mode has no lane-balance requirement and therefore cannot provide
  // a valid lane-balance proof. Callers gate only when `required` is true.
  if (!result.required) {
    return result;
  }

  if (!result.depth_valid || !result.tolerance_valid) {
    result.valid = false;
    result.topology_complete = false;
    result.every_lane_active = false;
    result.within_tolerance = false;
    return result;
  }

  result.expected_lanes = std::uint64_t{1} << depth;
  result.topology_complete = result.observed_lanes == result.expected_lanes;
  result.every_lane_active = result.topology_complete &&
                             std::all_of(lane_measured_transfers.begin(), lane_measured_transfers.end(),
                                         [](std::uint64_t count) { return count != 0; });
  result.within_tolerance = result.topology_complete && aggregate_measured_transfers != 0;

  if (aggregate_measured_transfers != 0) {
    result.min_equal_share_bps = detail::equal_share_bps(
        result.min_measured_transfers, result.expected_lanes, aggregate_measured_transfers);
    result.max_equal_share_bps = detail::equal_share_bps(
        result.max_measured_transfers, result.expected_lanes, aggregate_measured_transfers);
  }

  if (result.within_tolerance) {
    for (auto measured_transfers : lane_measured_transfers) {
      if (detail::compare_three_factor_product(
              measured_transfers, result.expected_lanes, canonical_lane_equal_share_bps,
              aggregate_measured_transfers, result.minimum_allowed_equal_share_bps) < 0 ||
          detail::compare_three_factor_product(
              measured_transfers, result.expected_lanes, canonical_lane_equal_share_bps,
              aggregate_measured_transfers, result.maximum_allowed_equal_share_bps) > 0) {
        result.within_tolerance = false;
        break;
      }
    }
  }
  result.valid = result.topology_complete && result.totals_reconcile &&
                 result.every_lane_active && result.within_tolerance;
  return result;
}

// A NativeTransferRun has one source signature for a contiguous, ordered
// nonce interval. Keep this small model independent from the wire codec so
// generator policy tests can protect dispatcher range rules without needing
// to serialize a v5 message. The generator has a compile-time guard against
// this value drifting from block::NativeTransferRun::max_entries.
constexpr std::uint32_t max_native_signed_run_entries = 16;

// These are physical-body limits of liteServer.sendMessageBatch. Logical
// transfer credit remains an independent, usually smaller client/worker bound.
constexpr std::size_t max_submission_batch_bodies = 1024;
constexpr std::size_t max_submission_batch_bytes = 8 << 20;

struct NativeRunBatchingSettings {
  // Existing NTRN commands often pass a formerly ignored scalar batch size.
  // Require this separate opt-in to preserve their individual-send behavior.
  bool requested{false};
};

inline bool native_run_batching_enabled(const NativeRunBatchingSettings& settings,
                                        bool native_signed_runs, std::uint32_t batch_size) {
  return settings.requested && native_signed_runs && batch_size > 1;
}

inline bool valid_native_run_batching_configuration(const NativeRunBatchingSettings& settings,
                                                    bool native_signed_runs, std::uint32_t batch_size) {
  return !settings.requested || native_run_batching_enabled(settings, native_signed_runs, batch_size);
}

inline bool valid_submission_batch_timeout(bool native_signed_runs,
                                           const NativeRunBatchingSettings& settings,
                                           std::uint32_t batch_size, double seconds) {
  auto uses_batches = (!native_signed_runs && batch_size > 1) ||
                      native_run_batching_enabled(settings, native_signed_runs, batch_size);
  return !uses_batches || seconds >= 9.0;
}

// One ready source head contributes one intact signed parent. Rejected
// additions leave all budgets unchanged so the actor can requeue that head.
// This policy never waits for more work or manufactures a smaller parent.
class NativeSignedRunBatchBudget {
 public:
  NativeSignedRunBatchBudget(std::size_t body_limit, std::uint32_t worker_credit,
                            std::uint32_t client_credit)
      : logical_capacity_(std::min(worker_credit, client_credit)),
        body_limit_(std::min({body_limit, max_submission_batch_bodies,
                              static_cast<std::size_t>(logical_capacity_)})) {
    sources_.reserve(body_limit_);
  }

  bool try_append(std::size_t source, std::uint32_t logical_count,
                  std::size_t body_bytes) {
    if (full() || logical_count == 0 || logical_count > max_native_signed_run_entries ||
        logical_count > logical_capacity_ - logical_count_ || body_bytes == 0 ||
        body_bytes > max_submission_batch_bytes - body_bytes_ ||
        std::find(sources_.begin(), sources_.end(), source) != sources_.end()) {
      return false;
    }
    sources_.push_back(source);
    logical_count_ += logical_count;
    body_bytes_ += body_bytes;
    return true;
  }

  bool full() const {
    return sources_.size() >= body_limit_ || logical_count_ == logical_capacity_ ||
           body_bytes_ == max_submission_batch_bytes;
  }
  std::size_t body_limit() const { return body_limit_; }
  std::size_t body_count() const { return sources_.size(); }
  std::uint32_t logical_count() const { return logical_count_; }
  std::size_t body_bytes() const { return body_bytes_; }
  const std::vector<std::size_t>& sources() const { return sources_; }

 private:
  std::uint32_t logical_capacity_;
  std::size_t body_limit_;
  std::uint32_t logical_count_{0};
  std::size_t body_bytes_{0};
  std::vector<std::size_t> sources_;
};

struct NativeSignedRunSettings {
  // Keep this opt-in so the established scalar NTFX generator remains the
  // default. When requested, the generator emits one NTRN authorization for
  // each plan rather than expanding that interval into scalar messages.
  bool requested{false};
  std::uint32_t entries_per_run{max_native_signed_run_entries};
};

struct NativeSignedRunPlan {
  std::uint64_t first_nonce{0};
  std::uint32_t logical_count{0};

  bool is_valid() const {
    return logical_count != 0 && logical_count <= max_native_signed_run_entries &&
           first_nonce <= std::numeric_limits<std::uint64_t>::max() - (logical_count - 1);
  }

  // Keep all range arithmetic overflow-safe.  A run may legally include
  // UINT64_MAX as its final nonce, so callers must not manufacture an
  // exclusive uint64 end marker merely to test containment or completion.
  bool contains(std::uint64_t nonce) const {
    return is_valid() && nonce >= first_nonce &&
           nonce - first_nonce < static_cast<std::uint64_t>(logical_count);
  }

  // `observed_next_nonce` is the account's next expected nonce.  It proves
  // this run only when it is strictly beyond every member of the interval.
  // A progress value inside the interval is a protocol violation for an
  // atomic NTRN run and must never cause a child-only retry or resolution.
  bool completed_before(std::uint64_t observed_next_nonce) const {
    return is_valid() && observed_next_nonce > first_nonce &&
           observed_next_nonce - first_nonce >= static_cast<std::uint64_t>(logical_count);
  }

  bool bisected_by(std::uint64_t observed_next_nonce) const {
    return is_valid() && observed_next_nonce > first_nonce &&
           !completed_before(observed_next_nonce);
  }
};

enum class NativeSignedRunIssueHoldReason {
  none,
  active_capacity,
  source_capacity,
  canonical_capacity,
  pacing_credit,
  client_capacity,
  query_credit,
};

inline bool valid_native_signed_run_entries_per_run(std::uint32_t entries_per_run) {
  return entries_per_run >= 1 && entries_per_run <= max_native_signed_run_entries;
}

inline bool valid_native_signed_run_settings(const NativeSignedRunSettings& settings) {
  return valid_native_signed_run_entries_per_run(settings.entries_per_run);
}

// Normal source issuance uses one stable authorization quantum. The caller
// combines static worker, source, canonical, and per-client ceilings so an
// oversized configuration is reduced once instead of creating work which can
// never be issued or dispatched.
inline std::uint32_t effective_native_signed_run_quantum(
    const NativeSignedRunSettings& settings, std::uint32_t static_capacity_ceiling) {
  if (!settings.requested || !valid_native_signed_run_settings(settings) ||
      static_capacity_ceiling == 0) {
    return 0;
  }
  return std::min(settings.entries_per_run, static_capacity_ceiling);
}

inline bool native_signed_run_source_capacity_exhausted(
    std::uint64_t outstanding, std::uint64_t source_limit,
    std::uint32_t quantum) {
  return quantum == 0 || outstanding >= source_limit ||
         source_limit - outstanding < quantum;
}

// Wallet::next_nonce is an exclusive uint64 cursor. Once a legal terminal
// tail advances it to UINT64_MAX, no further nonce can be represented without
// wrapping; the source remains live for proof and drain but not new issuance.
inline bool native_signed_run_nonce_cursor_exhausted(
    std::uint64_t next_nonce) {
  return next_nonce == std::numeric_limits<std::uint64_t>::max();
}

// Hold rather than resize an ordinary NTRN when a transient budget has fewer
// slots than its stable quantum.  The ordered reason is also a mutually
// exclusive telemetry taxonomy for one source-issue turn.
inline NativeSignedRunIssueHoldReason native_signed_run_issue_hold_reason(
    std::uint32_t required_logical_count, std::uint64_t active_capacity,
    std::uint64_t source_capacity, std::uint64_t canonical_capacity,
    bool paced, std::uint64_t pacing_credit,
    std::uint32_t largest_client_available_capacity,
    std::uint32_t largest_query_dispatchable_capacity) {
  if (required_logical_count == 0 || active_capacity < required_logical_count) {
    return NativeSignedRunIssueHoldReason::active_capacity;
  }
  if (source_capacity < required_logical_count) {
    return NativeSignedRunIssueHoldReason::source_capacity;
  }
  if (canonical_capacity < required_logical_count) {
    return NativeSignedRunIssueHoldReason::canonical_capacity;
  }
  if (paced && pacing_credit < required_logical_count) {
    return NativeSignedRunIssueHoldReason::pacing_credit;
  }
  if (largest_client_available_capacity < required_logical_count) {
    return NativeSignedRunIssueHoldReason::client_capacity;
  }
  if (largest_query_dispatchable_capacity < required_logical_count) {
    return NativeSignedRunIssueHoldReason::query_credit;
  }
  return NativeSignedRunIssueHoldReason::none;
}

// The token bucket must be able to accumulate at least one indivisible run,
// including at low target rates and during the start of a linear ramp.
inline double native_signed_run_pacing_burst_cap(double ordinary_burst_cap,
                                                  std::uint32_t quantum) {
  return std::max(ordinary_burst_cap, static_cast<double>(quantum));
}

// A signed parent cannot be split after signing, so its candidate interval
// must fit the remaining proof-observed canonical backlog budget before the
// issue decision admits its stable quantum. A disabled backlog control retains
// the established capacity calculation.
inline std::uint64_t bounded_native_signed_run_canonical_capacity(
    std::uint64_t logical_capacity, std::uint64_t canonical_backlog,
    std::uint64_t configured_backlog_limit, bool enforce_canonical_backlog) {
  if (!enforce_canonical_backlog || configured_backlog_limit == 0) {
    return logical_capacity;
  }
  if (canonical_backlog >= configured_backlog_limit) {
    return 0;
  }
  return std::min(logical_capacity, configured_backlog_limit - canonical_backlog);
}

// Match the ordinary issuance quantum when reporting canonical backpressure.
// A residual below one whole NTRN is blocked even when backlog < configured
// limit. Scalar issuance retains a quantum of one; drain never adds pause time.
inline bool canonical_backpressure_active(std::uint64_t canonical_backlog,
                                           std::uint64_t configured_backlog_limit,
                                           bool enforce_canonical_backlog, bool sending_done,
                                           std::uint32_t issue_quantum = 1) {
  auto required = std::max<std::uint32_t>(1, issue_quantum);
  return !sending_done &&
         bounded_native_signed_run_canonical_capacity(required, canonical_backlog,
                                                       configured_backlog_limit,
                                                       enforce_canonical_backlog) < required;
}

// A requested run consumes only a consecutive sequence which fits both the
// configured run bound and the nonce range. A short tail is valid, but the
// returned plan is always one indivisible authorization: later integration
// must never split it for batching, retry, checkpoint, or rollback.
inline NativeSignedRunPlan make_native_signed_run_plan(std::uint64_t first_nonce,
                                                       std::size_t contiguous_ready,
                                                       const NativeSignedRunSettings& settings) {
  if (!settings.requested || !valid_native_signed_run_settings(settings) || contiguous_ready == 0) {
    return {first_nonce, 0};
  }
  auto count = std::min<std::size_t>(contiguous_ready, settings.entries_per_run);
  while (count != 0 &&
         first_nonce > std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(count - 1)) {
    --count;
  }
  return {first_nonce, static_cast<std::uint32_t>(count)};
}

// Ordinary generation is full-quantum except for the finite nonce-domain tail.
// Wallet::next_nonce is an exclusive uint64 cursor, so UINT64_MAX itself is
// not issuable: the last representable plan advances the cursor exactly to it.
inline NativeSignedRunPlan make_native_signed_run_quantum_plan(
    std::uint64_t first_nonce, std::size_t logical_capacity,
    std::uint32_t quantum) {
  if (quantum == 0 || quantum > max_native_signed_run_entries ||
      first_nonce == std::numeric_limits<std::uint64_t>::max()) {
    return {first_nonce, 0};
  }
  auto nonce_capacity = std::numeric_limits<std::uint64_t>::max() - first_nonce;
  auto count = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(quantum, nonce_capacity));
  if (logical_capacity < count) {
    return {first_nonce, 0};
  }
  return {first_nonce, count};
}

inline NativeSignedRunPlan make_native_signed_run_repair_plan(
    std::uint64_t first_nonce, std::size_t remaining_logical,
    std::uint32_t quantum) {
  if (quantum == 0 || quantum > max_native_signed_run_entries) {
    return {first_nonce, 0};
  }
  NativeSignedRunSettings settings{true, quantum};
  return make_native_signed_run_plan(first_nonce, remaining_logical, settings);
}

inline bool native_signed_run_repair_capacity_available(
    std::uint32_t logical_count, std::uint64_t active_capacity,
    std::uint32_t largest_query_dispatchable_capacity) {
  return logical_count != 0 && active_capacity >= logical_count &&
         largest_query_dispatchable_capacity >= logical_count;
}

}  // namespace native_load
