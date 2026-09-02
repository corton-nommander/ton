#include <cmath>
#include <map>

#include "td/utils/tests.h"

#include "native-load-generator-policy.hpp"

TEST(NativeLoadGeneratorPolicy, AdaptiveCwndAckHonorsIndependentCeiling) {
  auto below = native_load::adaptive_cwnd_after_ack(63.0, 4096.0, 64.0);
  ASSERT_TRUE(below.cwnd > 63.0);
  ASSERT_TRUE(below.cwnd < 64.0);
  ASSERT_TRUE(!below.limited);

  auto boundary = native_load::adaptive_cwnd_after_ack(63.999, 4096.0, 64.0);
  ASSERT_TRUE(std::abs(boundary.cwnd - 64.0) < 1e-9);
  ASSERT_TRUE(boundary.limited);

  auto repeated = boundary;
  for (std::size_t i = 0; i < 1000; ++i) {
    repeated = native_load::adaptive_cwnd_after_ack(repeated.cwnd, 4096.0, 64.0);
  }
  ASSERT_TRUE(std::abs(repeated.cwnd - 64.0) < 1e-9);
  ASSERT_TRUE(repeated.limited);
}

TEST(NativeLoadGeneratorPolicy, ZeroAdaptiveCwndCeilingPreservesHardLimit) {
  auto result = native_load::adaptive_cwnd_after_ack(64.0, 4096.0, 0.0);
  ASSERT_TRUE(result.cwnd > 64.0);
  ASSERT_TRUE(!result.limited);

  auto hard_limited = native_load::adaptive_cwnd_after_ack(4096.0, 4096.0, 0.0);
  ASSERT_TRUE(std::abs(hard_limited.cwnd - 4096.0) < 1e-9);
  ASSERT_TRUE(hard_limited.limited);
}

TEST(NativeLoadGeneratorPolicy, AdaptiveCwndDistributionConservesGlobalCap) {
  std::uint32_t worker_total = 0;
  std::uint32_t client_total = 0;
  for (std::uint32_t worker = 0; worker < 6; ++worker) {
    auto worker_share = native_load::distributed_share(768, worker, 6);
    ASSERT_EQ(worker_share, 128u);
    worker_total += worker_share;
    for (std::uint32_t client = 0; client < 2; ++client) {
      auto client_share = native_load::distributed_share(worker_share, client, 2);
      ASSERT_EQ(client_share, 64u);
      client_total += client_share;
    }
  }
  ASSERT_EQ(worker_total, 768u);
  ASSERT_EQ(client_total, 768u);

  ASSERT_EQ(native_load::distributed_share(11, 0, 3), 4u);
  ASSERT_EQ(native_load::distributed_share(11, 1, 3), 4u);
  ASSERT_EQ(native_load::distributed_share(11, 2, 3), 3u);
  ASSERT_EQ(native_load::distributed_share(11, 3, 3), 0u);
  ASSERT_EQ(native_load::distributed_share(11, 0, 0), 0u);
}

TEST(NativeLoadGeneratorPolicy, AdaptiveCwndConfigurationBoundsAreExplicit) {
  ASSERT_TRUE(native_load::valid_adaptive_max_cwnd(0, 12, 65536));
  ASSERT_TRUE(native_load::valid_adaptive_max_cwnd(12, 12, 65536));
  ASSERT_TRUE(native_load::valid_adaptive_max_cwnd(768, 12, 65536));
  ASSERT_TRUE(native_load::valid_adaptive_max_cwnd(65536, 12, 65536));
  ASSERT_TRUE(!native_load::valid_adaptive_max_cwnd(11, 12, 65536));
  ASSERT_TRUE(!native_load::valid_adaptive_max_cwnd(65537, 12, 65536));
}

TEST(NativeLoadGeneratorPolicy, SubmitQueryCreditConfigurationAndZeroCompatibilityAreExplicit) {
  ASSERT_TRUE(native_load::valid_submit_max_queries_per_client(0));
  ASSERT_TRUE(native_load::valid_submit_max_queries_per_client(1));
  ASSERT_TRUE(native_load::valid_submit_max_queries_per_client(
      native_load::max_submit_queries_per_client));
  ASSERT_TRUE(!native_load::valid_submit_max_queries_per_client(
      native_load::max_submit_queries_per_client + 1));

  std::uint32_t queries_inflight = 0;
  for (std::uint32_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(native_load::client_can_dispatch_admission_query(1, 0, queries_inflight));
    ASSERT_TRUE(native_load::acquire_admission_query_credit(0, queries_inflight));
  }
  ASSERT_EQ(queries_inflight, 4u);
  ASSERT_TRUE(!native_load::admission_query_credit_at_cap(0, queries_inflight));
}

TEST(NativeLoadGeneratorPolicy, SubmitQueryCreditBlocksPartialBatchUntilOneReplyReleasesIt) {
  constexpr std::uint32_t query_cap = 1;
  std::uint32_t queries_inflight = 0;

  // A 35-message batch consumes one admission-query credit. The remaining
  // 29-message message window must not bypass a one-query per-client bound.
  ASSERT_TRUE(native_load::client_can_dispatch_admission_query(64, query_cap, queries_inflight));
  ASSERT_TRUE(native_load::acquire_admission_query_credit(query_cap, queries_inflight));
  ASSERT_EQ(queries_inflight, 1u);
  ASSERT_TRUE(native_load::admission_query_credit_at_cap(query_cap, queries_inflight));
  ASSERT_TRUE(!native_load::client_can_dispatch_admission_query(29, query_cap, queries_inflight));
  ASSERT_TRUE(!native_load::acquire_admission_query_credit(query_cap, queries_inflight));

  ASSERT_TRUE(native_load::release_admission_query_credit(queries_inflight));
  ASSERT_EQ(queries_inflight, 0u);
  ASSERT_TRUE(native_load::client_can_dispatch_admission_query(29, query_cap, queries_inflight));
}

TEST(NativeLoadGeneratorPolicy, SubmitQueryCreditGatesSingleAndFullBatchSelection) {
  constexpr std::uint32_t query_cap = 1;
  std::uint32_t queries_inflight = 0;
  ASSERT_TRUE(native_load::client_can_dispatch_admission_query(1, query_cap, queries_inflight));
  ASSERT_TRUE(native_load::client_can_dispatch_admission_query(64, query_cap, queries_inflight));

  ASSERT_TRUE(native_load::acquire_admission_query_credit(query_cap, queries_inflight));
  ASSERT_TRUE(!native_load::client_can_dispatch_admission_query(1, query_cap, queries_inflight));
  ASSERT_TRUE(!native_load::client_can_dispatch_admission_query(64, query_cap, queries_inflight));
  ASSERT_TRUE(native_load::release_admission_query_credit(queries_inflight));
}

TEST(NativeLoadGeneratorPolicy, SubmitQueryCreditIsPerClientAndNeverDistributed) {
  std::uint32_t first_client_queries = 0;
  std::uint32_t second_client_queries = 0;
  ASSERT_TRUE(native_load::acquire_admission_query_credit(1, first_client_queries));
  ASSERT_TRUE(native_load::acquire_admission_query_credit(1, second_client_queries));
  ASSERT_EQ(first_client_queries, 1u);
  ASSERT_EQ(second_client_queries, 1u);
}

TEST(NativeLoadGeneratorPolicy, SubmitQueryCreditReleasesOnceForErrorInactiveAndFinishedCallbacks) {
  std::uint32_t queries_inflight = 0;

  // A single-message transport error returns its one RPC credit before retry
  // handling.
  ASSERT_TRUE(native_load::acquire_admission_query_credit(2, queries_inflight));
  ASSERT_TRUE(native_load::release_admission_query_credit(queries_inflight));
  ASSERT_EQ(queries_inflight, 0u);

  // A post-finish single callback returns its credit before it observes the
  // finished state and exits.
  ASSERT_TRUE(native_load::acquire_admission_query_credit(2, queries_inflight));
  ASSERT_TRUE(native_load::release_admission_query_credit(queries_inflight));
  ASSERT_EQ(queries_inflight, 0u);

  // One failed batch is still one admission RPC even when every contained
  // transfer is inactive by the time the callback reaches it.
  ASSERT_TRUE(native_load::acquire_admission_query_credit(2, queries_inflight));
  ASSERT_TRUE(native_load::release_admission_query_credit(queries_inflight));
  ASSERT_EQ(queries_inflight, 0u);
  ASSERT_TRUE(!native_load::release_admission_query_credit(queries_inflight));
}

TEST(NativeLoadGeneratorPolicy, DetectsOnlyExplicitCanonicalStateLag) {
  ASSERT_TRUE(native_load::is_canonical_state_lag_diagnostic(
      "error 651: canonical native account state has not caught up with finalized balance"));
  ASSERT_TRUE(native_load::is_canonical_state_lag_diagnostic(
      "canonical native account state has not caught up with finalized balance; retry admission"));
  ASSERT_TRUE(native_load::is_canonical_state_lag_diagnostic(
      "error 651: native account state predates the latest observed canonical state"));
  ASSERT_TRUE(native_load::is_canonical_state_lag_diagnostic(
      "native account state predates the latest observed canonical state; retry admission"));

  ASSERT_TRUE(!native_load::is_canonical_state_lag_diagnostic("not ready"));
  ASSERT_TRUE(!native_load::is_canonical_state_lag_diagnostic(
      "native account changed before mempool insertion; retry admission"));
  ASSERT_TRUE(!native_load::is_canonical_state_lag_diagnostic("mempool is full"));
}

TEST(NativeLoadGeneratorPolicy, CanonicalLagBackoffIsBounded) {
  auto delay = [](std::uint32_t failures) {
    return native_load::canonical_state_lag_retry_delay_seconds(failures, 250, 2000);
  };
  ASSERT_TRUE(std::abs(delay(1) - 0.25) < 1e-9);
  ASSERT_TRUE(std::abs(delay(2) - 0.50) < 1e-9);
  ASSERT_TRUE(std::abs(delay(3) - 1.00) < 1e-9);
  ASSERT_TRUE(std::abs(delay(4) - 2.00) < 1e-9);
  ASSERT_TRUE(std::abs(delay(8) - 2.00) < 1e-9);
}

TEST(NativeLoadGeneratorPolicy, RetryHorizonUsesElapsedTime) {
  ASSERT_TRUE(!native_load::retry_horizon_elapsed(-1.0, 100.0, 30.0));
  ASSERT_TRUE(!native_load::retry_horizon_elapsed(100.0, 129.999, 30.0));
  ASSERT_TRUE(native_load::retry_horizon_elapsed(100.0, 130.0, 30.0));
  ASSERT_TRUE(native_load::retry_horizon_elapsed(100.0, 140.0, 30.0));
  ASSERT_TRUE(std::abs(native_load::clamp_retry_delay_to_horizon(25.6, 100.0, 125.5, 30.0) - 4.5) < 1e-9);
  ASSERT_TRUE(std::abs(native_load::clamp_retry_delay_to_horizon(2.0, 100.0, 110.0, 30.0) - 2.0) < 1e-9);
  ASSERT_TRUE(std::abs(native_load::clamp_retry_delay_to_horizon(2.0, 100.0, 130.0, 30.0)) < 1e-9);
}

TEST(NativeLoadGeneratorPolicy, ReadySourceRunIsBoundedAndRequeuesItsRemainder) {
  std::map<std::uint64_t, bool> tasks{{10, true}, {11, true}, {12, true}, {13, true}};
  native_load::ReadySourceQueue queue;
  queue.reset(1);
  ASSERT_TRUE(queue.enqueue(0));
  ASSERT_TRUE(!queue.enqueue(0));
  auto first = queue.pop();
  ASSERT_TRUE(first.kind == native_load::ReadySourceQueue::PopKind::ready);
  ASSERT_EQ(first.source_idx, 0u);
  ASSERT_EQ(native_load::bounded_contiguous_ready_run(tasks.begin(), tasks.end(), 3, [](bool ready) { return ready; }),
            3u);

  tasks.erase(10);
  tasks.erase(11);
  tasks.erase(12);
  ASSERT_TRUE(queue.enqueue(0));
  auto remainder = queue.pop();
  ASSERT_TRUE(remainder.kind == native_load::ReadySourceQueue::PopKind::ready);
  ASSERT_EQ(native_load::bounded_contiguous_ready_run(tasks.begin(), tasks.end(), 3, [](bool ready) { return ready; }),
            1u);
}

TEST(NativeLoadGeneratorPolicy, RetryingHeadWakesReadySuccessorsInNonceOrder) {
  std::map<std::uint64_t, bool> tasks{{20, false}, {21, true}, {22, true}};
  native_load::ReadySourceQueue queue;
  queue.reset(1);

  // A ready successor never queues the source while its unresolved head is
  // waiting to retry.
  if (tasks.begin()->second) {
    queue.enqueue(0);
  }
  ASSERT_TRUE(queue.empty());

  tasks.begin()->second = true;
  ASSERT_TRUE(queue.enqueue(0));
  auto retry = queue.pop();
  ASSERT_TRUE(retry.kind == native_load::ReadySourceQueue::PopKind::ready);
  ASSERT_EQ(native_load::bounded_contiguous_ready_run(tasks.begin(), tasks.end(), 8, [](bool ready) { return ready; }),
            3u);
}

TEST(NativeLoadGeneratorPolicy, StaleReadyGenerationCannotDispatch) {
  native_load::ReadySourceQueue queue;
  queue.reset(1);
  ASSERT_TRUE(queue.enqueue(0));
  queue.invalidate(0);
  ASSERT_TRUE(queue.enqueue(0));

  auto stale = queue.pop();
  ASSERT_TRUE(stale.kind == native_load::ReadySourceQueue::PopKind::stale);
  auto current = queue.pop();
  ASSERT_TRUE(current.kind == native_load::ReadySourceQueue::PopKind::ready);
  ASSERT_TRUE(queue.empty());
}

TEST(NativeLoadGeneratorPolicy, QuarantinedSourceCannotBeRequeuedByStaleRetry) {
  native_load::ReadySourceQueue queue;
  queue.reset(1);
  ASSERT_TRUE(queue.enqueue(0));
  queue.disable(0);

  ASSERT_EQ(native_load::active_tasks_after_source_quarantine(9, 4), 5u);
  ASSERT_TRUE(!queue.enqueue(0));
  ASSERT_TRUE(!native_load::retry_entry_can_wake(false, false, 130.0, 130.0));
  auto stale = queue.pop();
  ASSERT_TRUE(stale.kind == native_load::ReadySourceQueue::PopKind::stale);
  ASSERT_TRUE(queue.empty());
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunsAreDisabledByDefault) {
  native_load::NativeSignedRunSettings settings;
  ASSERT_TRUE(!settings.requested);
  ASSERT_TRUE(native_load::valid_native_signed_run_settings(settings));

  auto plan = native_load::make_native_signed_run_plan(100, 16, settings);
  ASSERT_EQ(plan.first_nonce, 100u);
  ASSERT_EQ(plan.logical_count, 0u);
  ASSERT_TRUE(!plan.is_valid());
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunPlanIsBoundedAndAllowsShortTail) {
  native_load::NativeSignedRunSettings settings;
  settings.requested = true;
  settings.entries_per_run = native_load::max_native_signed_run_entries;

  auto full = native_load::make_native_signed_run_plan(100, 64, settings);
  ASSERT_EQ(full.first_nonce, 100u);
  ASSERT_EQ(full.logical_count, native_load::max_native_signed_run_entries);
  ASSERT_TRUE(full.is_valid());

  auto tail = native_load::make_native_signed_run_plan(200, 3, settings);
  ASSERT_EQ(tail.first_nonce, 200u);
  ASSERT_EQ(tail.logical_count, 3u);
  ASSERT_TRUE(tail.is_valid());
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunPacingWaitsForTheBoundedAtomicTarget) {
  ASSERT_EQ(native_load::bounded_native_signed_run_pacing_target(16, 16, 50), 16u);
  ASSERT_TRUE(native_load::should_hold_native_signed_run_for_pacing(true, 9, 16));
  ASSERT_TRUE(!native_load::should_hold_native_signed_run_for_pacing(true, 16, 16));

  ASSERT_EQ(native_load::bounded_native_signed_run_pacing_target(7, 16, 50), 7u);
  ASSERT_TRUE(!native_load::should_hold_native_signed_run_for_pacing(true, 7, 7));

  ASSERT_EQ(native_load::bounded_native_signed_run_pacing_target(16, 16, 7), 7u);
  ASSERT_TRUE(native_load::should_hold_native_signed_run_for_pacing(true, 6, 7));
  ASSERT_TRUE(!native_load::should_hold_native_signed_run_for_pacing(false, 1, 16));
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunPlanCannotOverflowItsNonceInterval) {
  native_load::NativeSignedRunSettings settings;
  settings.requested = true;
  settings.entries_per_run = native_load::max_native_signed_run_entries;
  auto max_nonce = std::numeric_limits<std::uint64_t>::max();

  auto full = native_load::make_native_signed_run_plan(
      max_nonce - (native_load::max_native_signed_run_entries - 1), 16, settings);
  ASSERT_EQ(full.logical_count, native_load::max_native_signed_run_entries);
  ASSERT_TRUE(full.is_valid());

  auto truncated = native_load::make_native_signed_run_plan(max_nonce - 2, 16, settings);
  ASSERT_EQ(truncated.logical_count, 3u);
  ASSERT_TRUE(truncated.is_valid());

  auto final_nonce = native_load::make_native_signed_run_plan(max_nonce, 16, settings);
  ASSERT_EQ(final_nonce.logical_count, 1u);
  ASSERT_TRUE(final_nonce.is_valid());
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunRangeMustResolveAsOneWholeInterval) {
  native_load::NativeSignedRunSettings settings;
  settings.requested = true;
  settings.entries_per_run = 4;
  auto plan = native_load::make_native_signed_run_plan(40, 4, settings);

  ASSERT_TRUE(plan.is_valid());
  ASSERT_TRUE(plan.contains(40));
  ASSERT_TRUE(plan.contains(43));
  ASSERT_TRUE(!plan.contains(39));
  ASSERT_TRUE(!plan.contains(44));

  // A canonical/account-progress update through only child nonces 40..42 is
  // not a valid resolution point for a source-signed run.
  ASSERT_TRUE(!plan.completed_before(40));
  ASSERT_TRUE(plan.bisected_by(41));
  ASSERT_TRUE(plan.bisected_by(43));
  ASSERT_TRUE(!plan.completed_before(43));
  ASSERT_TRUE(plan.completed_before(44));
  ASSERT_TRUE(!plan.bisected_by(44));
}

TEST(NativeLoadGeneratorPolicy, SignedRunsKeepPhysicalAndLogicalTrafficCountersSeparate) {
  auto signed_submission = native_load::single_submission_message_counts(16);
  ASSERT_EQ(signed_submission.physical_messages, 1u);
  ASSERT_EQ(signed_submission.logical_transfers, 16u);

  auto scalar_batch = native_load::batch_submission_message_counts(96, 96);
  ASSERT_EQ(scalar_batch.physical_messages, 96u);
  ASSERT_EQ(scalar_batch.logical_transfers, 96u);

  auto signed_burst = native_load::source_issue_burst_message_counts(true, 16);
  ASSERT_EQ(signed_burst.physical_messages, 1u);
  ASSERT_EQ(signed_burst.logical_transfers, 16u);

  auto scalar_burst = native_load::source_issue_burst_message_counts(false, 16);
  ASSERT_EQ(scalar_burst.physical_messages, 16u);
  ASSERT_EQ(scalar_burst.logical_transfers, 16u);
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunSettingsRejectInvalidBounds) {
  native_load::NativeSignedRunSettings settings;
  settings.requested = true;
  settings.entries_per_run = 0;
  ASSERT_TRUE(!native_load::valid_native_signed_run_settings(settings));
  ASSERT_EQ(native_load::make_native_signed_run_plan(1, 8, settings).logical_count, 0u);

  settings.entries_per_run = native_load::max_native_signed_run_entries + 1;
  ASSERT_TRUE(!native_load::valid_native_signed_run_settings(settings));
  ASSERT_EQ(native_load::make_native_signed_run_plan(1, 8, settings).logical_count, 0u);
}

TEST(NativeLoadGeneratorPolicy, SubmitCoalescerHonorsTwoMillisecondDeadline) {
  native_load::SubmitCoalescer coalescer;
  coalescer.note_fresh_ready(10.0, 0.002);

  ASSERT_TRUE(coalescer.armed());
  ASSERT_TRUE(std::abs(coalescer.deadline() - 10.002) < 1e-9);
  ASSERT_TRUE(coalescer.release_reason(10.001999, 1, 64, false) ==
              native_load::SubmitCoalescer::ReleaseReason::blocked);
  ASSERT_TRUE(coalescer.release_reason(10.002, 1, 64, false) == native_load::SubmitCoalescer::ReleaseReason::deadline);
}

TEST(NativeLoadGeneratorPolicy, SubmitCoalescerHonorsTwentyMillisecondLeadingEdge) {
  native_load::SubmitCoalescer coalescer;
  coalescer.note_fresh_ready(20.0, 0.020);
  coalescer.note_fresh_ready(20.010, 0.020);

  // An unrelated 10 ms maintenance tick and a later completion must neither
  // bypass nor slide the first completion's bounded deadline.
  ASSERT_TRUE(std::abs(coalescer.deadline() - 20.020) < 1e-9);
  ASSERT_TRUE(coalescer.release_reason(20.010, 2, 64, false) == native_load::SubmitCoalescer::ReleaseReason::blocked);
  ASSERT_TRUE(coalescer.release_reason(20.019999, 2, 64, false) ==
              native_load::SubmitCoalescer::ReleaseReason::blocked);
  ASSERT_TRUE(coalescer.release_reason(20.020, 2, 64, false) == native_load::SubmitCoalescer::ReleaseReason::deadline);
}

TEST(NativeLoadGeneratorPolicy, SubmitCoalescerReleasesFullBatchAndUrgentWorkEarly) {
  native_load::SubmitCoalescer coalescer;
  coalescer.note_fresh_ready(30.0, 0.020);

  ASSERT_TRUE(coalescer.release_reason(30.001, 63, 64, false) == native_load::SubmitCoalescer::ReleaseReason::blocked);
  ASSERT_TRUE(coalescer.release_reason(30.001, 64, 64, false) ==
              native_load::SubmitCoalescer::ReleaseReason::full_batch);
  ASSERT_TRUE(coalescer.release_reason(30.001, 0, 64, true) == native_load::SubmitCoalescer::ReleaseReason::bypass);
  // Bypassing for urgent work does not consume or prematurely release the
  // ordinary first submissions that are still waiting in the same worker.
  ASSERT_TRUE(coalescer.release_reason(30.001, 1, 64, false) == native_load::SubmitCoalescer::ReleaseReason::blocked);
}

TEST(NativeLoadGeneratorPolicy, SubmitCoalescerCannotStarveAfterDeadline) {
  native_load::SubmitCoalescer coalescer;
  coalescer.note_fresh_ready(40.0, 0.020);
  coalescer.note_fresh_ready(40.005, 0.020);
  ASSERT_TRUE(coalescer.release_reason(40.020, 1, 64, false) == native_load::SubmitCoalescer::ReleaseReason::deadline);

  // A source head can remain ready while every client is capacity-bound.  Its
  // elapsed window stays open rather than being restarted by later pumps.
  ASSERT_TRUE(coalescer.release_reason(41.0, 1, 64, false) == native_load::SubmitCoalescer::ReleaseReason::deadline);

  coalescer.note_queue_empty();
  ASSERT_TRUE(!coalescer.armed());
  coalescer.note_fresh_ready(42.0, 0.020);
  ASSERT_TRUE(std::abs(coalescer.deadline() - 42.020) < 1e-9);
  ASSERT_TRUE(coalescer.release_reason(42.010, 1, 64, false) == native_load::SubmitCoalescer::ReleaseReason::blocked);
}
