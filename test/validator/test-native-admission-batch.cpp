#include <limits>
#include <numeric>

#include "td/utils/tests.h"
#include "validator/impl/native-admission-telemetry.h"
#include "interfaces/external-message.h"

namespace {

td::Bits256 from_hex(td::Slice value) {
  td::Bits256 result;
  CHECK(result.from_hex(value) == 256);
  return result;
}

}  // namespace

TEST(NativeAdmissionBatch, DeterministicSourceNonceHashOrder) {
  auto source_a = from_hex("1000000000000000000000000000000000000000000000000000000000000000");
  auto source_b = from_hex("2000000000000000000000000000000000000000000000000000000000000000");
  auto hash_a = from_hex("0100000000000000000000000000000000000000000000000000000000000000");
  auto hash_b = from_hex("0200000000000000000000000000000000000000000000000000000000000000");

  std::vector<ton::validator::NativeAdmissionOrderKey> keys{
      {.workchain = ton::basechainId, .source = source_b, .nonce = 0, .hash = hash_a, .input_index = 4},
      {.workchain = ton::basechainId, .source = source_a, .nonce = 2, .hash = hash_a, .input_index = 1},
      {.workchain = ton::basechainId, .source = source_a, .nonce = 1, .hash = hash_b, .input_index = 2},
      {.workchain = ton::basechainId, .source = source_a, .nonce = 1, .hash = hash_a, .input_index = 3},
  };
  auto order = ton::validator::order_native_admissions(std::move(keys));
  ASSERT_EQ(order.size(), 4u);
  ASSERT_EQ(order[0], 3u);
  ASSERT_EQ(order[1], 2u);
  ASSERT_EQ(order[2], 1u);
  ASSERT_EQ(order[3], 4u);
}

TEST(NativeAdmissionBatch, StableWireStatus) {
  auto success = ton::validator::ExternalMessageAdmissionResult::success();
  ASSERT_TRUE(success.accepted);
  ASSERT_EQ(success.error_code, 0);
  ASSERT_TRUE(success.error_message.empty());

  auto failure = ton::validator::ExternalMessageAdmissionResult::failure(td::Status::Error(503, "deadline"));
  ASSERT_TRUE(!failure.accepted);
  ASSERT_EQ(failure.error_code, 503);
  ASSERT_EQ(failure.error_message, "deadline");
}

TEST(NativeAdmissionBatch, NotReadyCauseClassification) {
  using Cause = ton::validator::NativeAdmissionNotReadyCause;
  const std::pair<std::string_view, Cause> cases[]{
      {"native transfer run admission snapshot changed; retry", Cause::snapshot_changed},
      {"native account changed during signature verification; retry admission", Cause::account_changed_verification},
      {"native account changed before mempool insertion; retry admission", Cause::account_changed_insertion},
      {"native account changed before mempool commit; retry admission", Cause::account_changed_commit},
      {"native account state predates the latest observed canonical state", Cause::canonical_watermark_lag},
      {"native admission masterchain state is not ready", Cause::masterchain_unavailable},
      {"native admission masterchain state is invalid", Cause::masterchain_unavailable},
      {"native admission configuration was not pinned; retry", Cause::masterchain_unavailable},
      {"cannot locate native source shard in pinned masterchain state", Cause::shard_unavailable},
      {"cannot locate native transfer source shard in applied masterchain state", Cause::shard_unavailable},
      {"cannot locate native transfer destination shard in applied masterchain state", Cause::shard_unavailable},
      {"external message mempool is full (priority=0, size=8192)", Cause::mempool_full},
      {"external message per-address mempool limit reached (address=0:abc)", Cause::per_address_limit},
      {"not ready", Cause::other},
      {"native account changed for an unknown reason", Cause::other},
      {"external message mempool is full", Cause::other},
      {"", Cause::other},
  };
  for (const auto& [message, cause] : cases) {
    ASSERT_EQ(static_cast<unsigned>(ton::validator::classify_native_admission_not_ready(message)),
              static_cast<unsigned>(cause));
  }
}

TEST(NativeAdmissionBatch, FinalPhysicalOutcomesRemainDistinctFromLogicalChildren) {
  using Cause = ton::validator::NativeAdmissionNotReadyCause;
  using Result = ton::validator::ExternalMessageAdmissionResult;
  ton::validator::NativeAdmissionBatchTelemetry telemetry;
  const auto snapshot_error = Result::failure(td::Status::Error(
      ton::ErrorCode::notready, "native transfer run admission snapshot changed; retry"));
  // This is the final wire-indexed vector: one successful NTRN, a rejected
  // NTRN and its duplicate input, a timeout, and a different not-ready origin.
  // No logical child expansion or intermediate rejection contributes here.
  const std::vector<Result> statuses{
      Result::success(), snapshot_error, snapshot_error,
      Result::failure(td::Status::Error(ton::ErrorCode::timeout, snapshot_error.error_message)),
      Result::failure(td::Status::Error(ton::ErrorCode::notready, "manager unavailable"))};
  for (const auto& status : statuses) {
    telemetry.record_result(status.accepted, status.error_code, status.error_message);
  }
  // Even inconsistent successful fields cannot contribute a rejection.
  telemetry.record_result(true, ton::ErrorCode::notready, snapshot_error.error_message);
  ASSERT_EQ(telemetry.not_ready_total, 3u);
  ASSERT_EQ(telemetry.not_ready[static_cast<unsigned>(Cause::snapshot_changed)], 2u);
  ASSERT_EQ(telemetry.not_ready[static_cast<unsigned>(Cause::other)], 1u);
  ASSERT_EQ(std::accumulate(telemetry.not_ready.begin(), telemetry.not_ready.end(), std::uint64_t{0}),
            telemetry.not_ready_total);

  // A later request is a separate physical attempt even with the same error.
  telemetry.record_result(false, snapshot_error.error_code, snapshot_error.error_message);
  ASSERT_EQ(telemetry.not_ready_total, 4u);
  ASSERT_EQ(telemetry.not_ready[static_cast<unsigned>(Cause::snapshot_changed)], 3u);
}

TEST(NativeAdmissionBatch, MissingMasterchainContextDoesNotHideManagerErrors) {
  using Cause = ton::validator::NativeAdmissionNotReadyCause;
  ton::validator::NativeAdmissionBatchTelemetry telemetry;
  telemetry.record_result(false, ton::ErrorCode::notready, "not ready", true);
  telemetry.record_result(false, ton::ErrorCode::notready, "not ready");
  telemetry.record_result(false, ton::ErrorCode::timeout, "not ready", true);
  ASSERT_EQ(telemetry.not_ready_total, 2u);
  ASSERT_EQ(telemetry.not_ready[static_cast<unsigned>(Cause::masterchain_unavailable)], 1u);
  ASSERT_EQ(telemetry.not_ready[static_cast<unsigned>(Cause::other)], 1u);
}

TEST(NativeAdmissionBatch, OverlappingResidenceAndAbortedResultsHaveSeparateScopes) {
  ton::validator::NativeAdmissionBatchTelemetry telemetry;
  telemetry.begin_batch();
  telemetry.begin_batch();
  ASSERT_EQ(telemetry.active_batches, 2u);
  ASSERT_EQ(telemetry.peak_active_batches, 2u);
  telemetry.shard_wait.observe(0.125);
  telemetry.finish_batch(0.25, true);
  ASSERT_EQ(telemetry.active_batches, 1u);
  telemetry.begin_batch();
  telemetry.verification.observe(0.5);
  telemetry.finish_batch(0.75, false);
  telemetry.finish_batch(1.0, true);
  ASSERT_EQ(telemetry.active_batches, 0u);
  ASSERT_EQ(telemetry.peak_active_batches, 2u);
  ASSERT_EQ(telemetry.finished_batches, 2u);
  ASSERT_EQ(telemetry.aborted_batches, 1u);
  ASSERT_EQ(telemetry.residence.samples, 2u);
  ASSERT_EQ(telemetry.residence.sum_seconds, 1.25);
  ASSERT_EQ(telemetry.residence.max_seconds, 1.0);
  ASSERT_EQ(telemetry.aborted_residence.samples, 1u);
  ASSERT_EQ(telemetry.aborted_residence.sum_seconds, 0.75);
  // Completed stages survive a later abort, but it has no final status vector.
  ASSERT_EQ(telemetry.shard_wait.samples, 1u);
  ASSERT_EQ(telemetry.verification.samples, 1u);
  ASSERT_EQ(telemetry.not_ready_total, 0u);
}

TEST(NativeAdmissionBatch, WallSamplesIgnoreInvalidDurations) {
  ton::validator::NativeAdmissionWallTime time;
  time.observe(0.0);
  time.observe(0.25);
  time.observe(0.5);
  time.observe(-1.0);
  time.observe(std::numeric_limits<double>::infinity());
  time.observe(std::numeric_limits<double>::quiet_NaN());
  ASSERT_EQ(time.samples, 3u);
  ASSERT_EQ(time.sum_seconds, 0.75);
  ASSERT_EQ(time.max_seconds, 0.5);
}
