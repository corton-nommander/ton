#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "common/errorcode.h"

namespace ton::validator {

// Classification is telemetry only. Callers retain the original status code
// and diagnostic. Unknown diagnostics stay visible in the other counter.
enum class NativeAdmissionNotReadyCause : unsigned {
  snapshot_changed,
  account_changed_verification,
  account_changed_insertion,
  account_changed_commit,
  canonical_watermark_lag,
  masterchain_unavailable,
  shard_unavailable,
  mempool_full,
  per_address_limit,
  other,
  count
};

inline NativeAdmissionNotReadyCause classify_native_admission_not_ready(std::string_view message) {
  using Cause = NativeAdmissionNotReadyCause;
  if (message == "native transfer run admission snapshot changed; retry") {
    return Cause::snapshot_changed;
  }
  if (message == "native account changed during signature verification; retry admission") {
    return Cause::account_changed_verification;
  }
  if (message == "native account changed before mempool insertion; retry admission") {
    return Cause::account_changed_insertion;
  }
  if (message == "native account changed before mempool commit; retry admission") {
    return Cause::account_changed_commit;
  }
  if (message == "native account state predates the latest observed canonical state") {
    return Cause::canonical_watermark_lag;
  }
  if (message == "native admission masterchain state is not ready" ||
      message == "native admission masterchain state is invalid" ||
      message == "native admission configuration was not pinned; retry") {
    return Cause::masterchain_unavailable;
  }
  if (message == "cannot locate native source shard in pinned masterchain state" ||
      message == "cannot locate native transfer source shard in applied masterchain state" ||
      message == "cannot locate native transfer destination shard in applied masterchain state") {
    return Cause::shard_unavailable;
  }
  constexpr std::string_view mempool_full = "external message mempool is full (";
  if (message.substr(0, mempool_full.size()) == mempool_full) {
    return Cause::mempool_full;
  }
  constexpr std::string_view per_address = "external message per-address mempool limit reached (";
  if (message.substr(0, per_address.size()) == per_address) {
    return Cause::per_address_limit;
  }
  return Cause::other;
}

struct NativeAdmissionWallTime {
  std::uint64_t samples{0};
  double sum_seconds{0};
  double max_seconds{0};

  void observe(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) {
      return;
    }
    ++samples;
    sum_seconds += seconds;
    max_seconds = std::max(max_seconds, seconds);
  }
};

// One completed status is one physical input body, including duplicate input
// bodies. Retries are separate calls. No partial status vector is counted on
// coroutine abandonment. These are pool results, not confirmed RPC deliveries.
// Residence starts at pool coroutine entry, excluding upstream coalescer/mailbox
// waiting. Each completed stage contributes a wall-time sample even if a later
// stage aborts; verification samples require at least one signature task, and
// snapshot-age samples require reaching the post-verification snapshot check.
struct NativeAdmissionBatchTelemetry {
  static constexpr std::array<const char*, static_cast<unsigned>(NativeAdmissionNotReadyCause::count)> cause_names{
      "not_ready_snapshot_changed", "not_ready_account_changed_verification",
      "not_ready_account_changed_insertion", "not_ready_account_changed_commit",
      "not_ready_canonical_watermark_lag", "not_ready_masterchain_unavailable",
      "not_ready_shard_unavailable", "not_ready_mempool_full", "not_ready_per_address_limit", "not_ready_other"};

  std::array<std::uint64_t, cause_names.size()> not_ready{};
  std::uint64_t not_ready_total{0};
  std::uint64_t active_batches{0}, peak_active_batches{0}, finished_batches{0}, aborted_batches{0};
  NativeAdmissionWallTime decode;
  NativeAdmissionWallTime reservation;
  NativeAdmissionWallTime residence;
  NativeAdmissionWallTime aborted_residence;
  NativeAdmissionWallTime shard_wait;
  NativeAdmissionWallTime verification;
  NativeAdmissionWallTime snapshot_age;
  NativeAdmissionWallTime changed_snapshot_age;

  void begin_batch() {
    ++active_batches;
    peak_active_batches = std::max(peak_active_batches, active_batches);
  }

  void finish_batch(double seconds, bool completed) {
    if (active_batches == 0) {
      return;
    }
    --active_batches;
    if (completed) {
      ++finished_batches;
      residence.observe(seconds);
    } else {
      ++aborted_batches;
      aborted_residence.observe(seconds);
    }
  }

  // The missing-state entry branch uses a legacy generic diagnostic; its
  // explicit context must not classify unrelated manager "not ready" errors.
  void record_result(bool accepted, int error_code, std::string_view message, bool missing_masterchain = false) {
    if (accepted || error_code != ErrorCode::notready) {
      return;
    }
    record_not_ready(missing_masterchain ? NativeAdmissionNotReadyCause::masterchain_unavailable
                                        : classify_native_admission_not_ready(message));
  }

 private:
  void record_not_ready(NativeAdmissionNotReadyCause cause) {
    const auto index = static_cast<unsigned>(cause);
    if (index >= not_ready.size()) {
      return;
    }
    ++not_ready_total;
    ++not_ready[index];
  }
};

}  // namespace ton::validator
