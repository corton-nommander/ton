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

// AIMD windows count messages, not wire queries. A zero configured limit keeps
// the historical hard-limit-only behavior; otherwise every ACK grows toward
// the independently distributed admission ceiling without changing decrease
// semantics.
inline AdaptiveCwndAckResult adaptive_cwnd_after_ack(double current, double hard_limit, double configured_limit) {
  auto limit = configured_limit > 0.0 ? std::min(hard_limit, configured_limit) : hard_limit;
  auto wanted = current + 1.0 / std::max(1.0, current);
  return {.cwnd = std::min(limit, wanted), .limited = wanted > limit};
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

}  // namespace native_load
