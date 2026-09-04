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

TEST(NativeLoadGeneratorPolicy, AdaptiveCwndLossPreservesAtomicDispatchQuantum) {
  auto ordinary_loss = native_load::adaptive_cwnd_after_loss(64.0, 16.0);
  ASSERT_TRUE(std::abs(ordinary_loss.cwnd - 32.0) < 1e-9);
  ASSERT_TRUE(!ordinary_loss.minimum_limited);

  auto bounded_loss = native_load::adaptive_cwnd_after_loss(20.0, 16.0);
  ASSERT_TRUE(std::abs(bounded_loss.cwnd - 16.0) < 1e-9);
  ASSERT_TRUE(bounded_loss.minimum_limited);

  auto floor_loss = native_load::adaptive_cwnd_after_loss(16.0, 16.0);
  ASSERT_TRUE(std::abs(floor_loss.cwnd - 16.0) < 1e-9);
  ASSERT_TRUE(floor_loss.minimum_limited);

  // A client whose static ceiling is below the configured run size uses its
  // bounded effective quantum, while scalar admission retains a floor of one.
  ASSERT_TRUE(std::abs(native_load::adaptive_cwnd_after_loss(8.0, 8.0).cwnd - 8.0) < 1e-9);
  ASSERT_TRUE(std::abs(native_load::adaptive_cwnd_after_loss(1.0, 1.0).cwnd - 1.0) < 1e-9);
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

TEST(NativeLoadGeneratorPolicy, SignedRunSnapshotRevisionRacesAvoidOnlyAimdDecrease) {
  auto is_race = [](std::string_view diagnostic, bool signed_run_task = true) {
    return native_load::is_native_signed_run_admission_snapshot_or_revision_race_diagnostic(
        signed_run_task, diagnostic);
  };
  ASSERT_TRUE(is_race("error 651: native transfer run admission snapshot changed; retry"));
  ASSERT_TRUE(is_race("native account changed during signature verification; retry admission"));
  ASSERT_TRUE(is_race("native account changed before mempool insertion; retry admission"));
  ASSERT_TRUE(is_race("native account changed before mempool commit; retry admission"));

  // Do not turn a broad native/not-ready phrase into an AIMD exemption, and
  // retain the scalar generator's historical response to these diagnostics.
  ASSERT_TRUE(!is_race("native transfer run admission snapshot changed"));
  ASSERT_TRUE(!is_race("native account changed during signature verification; retry"));
  ASSERT_TRUE(!is_race("native account changed before mempool insertion; retry"));
  ASSERT_TRUE(!is_race("native account changed before mempool insertion; retry admission", false));
  ASSERT_TRUE(!is_race("not ready"));
  ASSERT_TRUE(!is_race("still in flight"));
  ASSERT_TRUE(!is_race("mempool is full"));

  auto should_decrease = [](bool timeout, bool full, bool rate_limit, bool not_ready,
                            bool canonical_state_lag, bool signed_run_race) {
    return native_load::should_decrease_adaptive_cwnd_for_admission_failure(
        timeout, full, rate_limit, not_ready, canonical_state_lag, signed_run_race);
  };
  ASSERT_TRUE(!should_decrease(false, false, false, true, false, true));
  ASSERT_TRUE(!should_decrease(false, false, false, true, true, false));
  ASSERT_TRUE(should_decrease(false, false, false, true, false, false));
  ASSERT_TRUE(should_decrease(true, false, false, true, false, true));
  ASSERT_TRUE(should_decrease(false, true, false, true, false, true));
  ASSERT_TRUE(should_decrease(false, false, true, true, false, true));
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

TEST(NativeLoadGeneratorPolicy, NativeSignedRunNormalQuantumIsStableAndDispatchable) {
  native_load::NativeSignedRunSettings settings;
  settings.requested = true;
  settings.entries_per_run = 16;

  ASSERT_EQ(native_load::effective_native_signed_run_quantum(settings, 64), 16u);
  ASSERT_EQ(native_load::effective_native_signed_run_quantum(settings, 8), 8u);
  ASSERT_EQ(native_load::effective_native_signed_run_quantum(settings, 0), 0u);

  auto full = native_load::make_native_signed_run_quantum_plan(100, 64, 16);
  ASSERT_TRUE(full.is_valid());
  ASSERT_EQ(full.logical_count, 16u);

  // Transient residual budgets do not resize an ordinary authorization.
  auto held = native_load::make_native_signed_run_quantum_plan(100, 15, 16);
  ASSERT_TRUE(!held.is_valid());
  ASSERT_EQ(held.logical_count, 0u);

  settings.requested = false;
  ASSERT_EQ(native_load::effective_native_signed_run_quantum(settings, 64), 0u);
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunSourceEligibilityRequiresWholeQuantum) {
  ASSERT_TRUE(!native_load::native_signed_run_source_capacity_exhausted(47, 63, 16));
  ASSERT_TRUE(native_load::native_signed_run_source_capacity_exhausted(48, 63, 16));
  ASSERT_TRUE(native_load::native_signed_run_source_capacity_exhausted(63, 63, 16));
  ASSERT_TRUE(native_load::native_signed_run_source_capacity_exhausted(0, 63, 0));
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunNormalPlanHandlesTerminalNonceTail) {
  auto max_nonce = std::numeric_limits<std::uint64_t>::max();
  auto terminal = native_load::make_native_signed_run_quantum_plan(
      max_nonce - 3, 3, 16);
  ASSERT_TRUE(terminal.is_valid());
  ASSERT_EQ(terminal.first_nonce, max_nonce - 3);
  ASSERT_EQ(terminal.logical_count, 3u);
  ASSERT_TRUE(!native_load::native_signed_run_nonce_cursor_exhausted(
      terminal.first_nonce));
  auto advanced_cursor = terminal.first_nonce + terminal.logical_count;
  ASSERT_EQ(advanced_cursor, max_nonce);
  ASSERT_TRUE(native_load::native_signed_run_nonce_cursor_exhausted(
      advanced_cursor));

  ASSERT_TRUE(!native_load::make_native_signed_run_quantum_plan(
                   max_nonce - 3, 2, 16)
                   .is_valid());
  ASSERT_TRUE(!native_load::make_native_signed_run_quantum_plan(
                   max_nonce, 16, 16)
                   .is_valid());
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunIssueHoldsUseOneOrderedReason) {
  using Reason = native_load::NativeSignedRunIssueHoldReason;
  constexpr std::uint32_t quantum = 16;

  ASSERT_TRUE(native_load::native_signed_run_issue_hold_reason(
                  quantum, 15, 15, 15, true, 15, 15, 15) ==
              Reason::active_capacity);
  ASSERT_TRUE(native_load::native_signed_run_issue_hold_reason(
                  quantum, 16, 15, 15, true, 15, 15, 15) ==
              Reason::source_capacity);
  ASSERT_TRUE(native_load::native_signed_run_issue_hold_reason(
                  quantum, 16, 16, 15, true, 15, 15, 15) ==
              Reason::canonical_capacity);
  ASSERT_TRUE(native_load::native_signed_run_issue_hold_reason(
                  quantum, 16, 16, 16, true, 15, 15, 15) ==
              Reason::pacing_credit);
  ASSERT_TRUE(native_load::native_signed_run_issue_hold_reason(
                  quantum, 16, 16, 16, true, 16, 15, 15) ==
              Reason::client_capacity);
  ASSERT_TRUE(native_load::native_signed_run_issue_hold_reason(
                  quantum, 16, 16, 16, true, 16, 16, 15) ==
              Reason::query_credit);
  ASSERT_TRUE(native_load::native_signed_run_issue_hold_reason(
                  quantum, 16, 16, 16, true, 16, 16, 16) == Reason::none);

  // Occupancy can temporarily leave one free slot in a 64-slot client. The
  // decision waits for sixteen; it never turns that residue into a 1-run.
  ASSERT_TRUE(native_load::native_signed_run_issue_hold_reason(
                  quantum, 64, 64, 64, false, 0, 1, 1) ==
              Reason::client_capacity);
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunRepairAloneMayUseFiniteTail) {
  auto full = native_load::make_native_signed_run_repair_plan(100, 64, 16);
  ASSERT_TRUE(full.is_valid());
  ASSERT_EQ(full.logical_count, 16u);

  auto tail = native_load::make_native_signed_run_repair_plan(116, 3, 16);
  ASSERT_TRUE(tail.is_valid());
  ASSERT_EQ(tail.logical_count, 3u);

  auto empty = native_load::make_native_signed_run_repair_plan(119, 0, 16);
  ASSERT_TRUE(!empty.is_valid());

  ASSERT_TRUE(native_load::native_signed_run_repair_capacity_available(
      16, 16, 16));
  ASSERT_TRUE(!native_load::native_signed_run_repair_capacity_available(
      16, 15, 16));
  ASSERT_TRUE(!native_load::native_signed_run_repair_capacity_available(
      16, 16, 15));
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunPacingBucketFitsOneQuantum) {
  ASSERT_TRUE(std::abs(native_load::native_signed_run_pacing_burst_cap(1.0, 16) - 16.0) < 1e-9);
  ASSERT_TRUE(std::abs(native_load::native_signed_run_pacing_burst_cap(500.0, 16) - 500.0) < 1e-9);
}

TEST(NativeLoadGeneratorPolicy, NativeSignedRunHoldsAtCanonicalBacklogTail) {
  // One remaining canonical slot cannot admit part of a 16-output normal
  // quantum. Progress will free the rest of the quantum; repair owns tails.
  auto remaining = native_load::bounded_native_signed_run_canonical_capacity(16, 0, 1, true);
  ASSERT_EQ(remaining, 1u);
  ASSERT_TRUE(native_load::native_signed_run_issue_hold_reason(
                  16, 16, 16, remaining, true, 16, 16, 16) ==
              native_load::NativeSignedRunIssueHoldReason::canonical_capacity);
  ASSERT_TRUE(!native_load::make_native_signed_run_quantum_plan(100, remaining, 16).is_valid());

  ASSERT_EQ(native_load::bounded_native_signed_run_canonical_capacity(16, 15, 16, true), 1u);
  ASSERT_EQ(native_load::bounded_native_signed_run_canonical_capacity(16, 16, 16, true), 0u);
  ASSERT_EQ(native_load::bounded_native_signed_run_canonical_capacity(16, 16, 0, true), 16u);
  ASSERT_EQ(native_load::bounded_native_signed_run_canonical_capacity(16, 16, 16, false), 16u);
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

TEST(NativeLoadGeneratorPolicy, CanonicalLaneBalanceAcceptsPerfectCompleteTopology) {
  auto balance = native_load::summarize_canonical_lane_balance(2, 400, {100, 100, 100, 100});

  ASSERT_TRUE(balance.required);
  ASSERT_TRUE(balance.valid);
  ASSERT_TRUE(balance.depth_valid);
  ASSERT_TRUE(balance.tolerance_valid);
  ASSERT_TRUE(balance.topology_complete);
  ASSERT_TRUE(balance.totals_reconcile);
  ASSERT_TRUE(balance.every_lane_active);
  ASSERT_TRUE(balance.within_tolerance);
  ASSERT_TRUE(!balance.measured_transfers_sum_overflow);
  ASSERT_EQ(balance.depth, 2u);
  ASSERT_EQ(balance.expected_lanes, 4u);
  ASSERT_EQ(balance.observed_lanes, 4u);
  ASSERT_EQ(balance.aggregate_measured_transfers, 400u);
  ASSERT_EQ(balance.measured_transfers_sum, 400u);
  ASSERT_EQ(balance.min_measured_transfers, 100u);
  ASSERT_EQ(balance.max_measured_transfers, 100u);
  ASSERT_EQ(balance.min_equal_share_bps, 10000u);
  ASSERT_EQ(balance.max_equal_share_bps, 10000u);
  ASSERT_EQ(balance.minimum_allowed_equal_share_bps, 9500u);
  ASSERT_EQ(balance.maximum_allowed_equal_share_bps, 10500u);
}

TEST(NativeLoadGeneratorPolicy, CanonicalLaneBalanceRejectsMissingAndStarvedLanes) {
  auto missing = native_load::summarize_canonical_lane_balance(2, 300, {100, 100, 100});
  ASSERT_TRUE(!missing.valid);
  ASSERT_TRUE(!missing.topology_complete);
  ASSERT_TRUE(missing.totals_reconcile);
  ASSERT_TRUE(!missing.every_lane_active);
  ASSERT_TRUE(!missing.within_tolerance);
  ASSERT_EQ(missing.expected_lanes, 4u);
  ASSERT_EQ(missing.observed_lanes, 3u);

  auto starved = native_load::summarize_canonical_lane_balance(2, 300, {0, 100, 100, 100});
  ASSERT_TRUE(!starved.valid);
  ASSERT_TRUE(starved.topology_complete);
  ASSERT_TRUE(starved.totals_reconcile);
  ASSERT_TRUE(!starved.every_lane_active);
  ASSERT_TRUE(!starved.within_tolerance);
  ASSERT_EQ(starved.min_measured_transfers, 0u);
  ASSERT_EQ(starved.min_equal_share_bps, 0u);
}

TEST(NativeLoadGeneratorPolicy, CanonicalLaneBalanceRejectsAggregateMismatch) {
  auto balance = native_load::summarize_canonical_lane_balance(2, 399, {100, 100, 100, 100});

  ASSERT_TRUE(!balance.valid);
  ASSERT_TRUE(balance.topology_complete);
  ASSERT_TRUE(!balance.totals_reconcile);
  ASSERT_TRUE(balance.every_lane_active);
  ASSERT_TRUE(balance.within_tolerance);
  ASSERT_EQ(balance.measured_transfers_sum, 400u);
  ASSERT_EQ(balance.aggregate_measured_transfers, 399u);
}

TEST(NativeLoadGeneratorPolicy, CanonicalLaneBalanceToleranceBoundariesAreInclusive) {
  auto boundary = native_load::summarize_canonical_lane_balance(
      2, 40000, {9500, 9500, 10500, 10500});
  ASSERT_TRUE(boundary.valid);
  ASSERT_TRUE(boundary.within_tolerance);
  ASSERT_EQ(boundary.min_equal_share_bps, 9500u);
  ASSERT_EQ(boundary.max_equal_share_bps, 10500u);

  auto below = native_load::summarize_canonical_lane_balance(
      2, 40000, {9499, 10167, 10167, 10167});
  ASSERT_TRUE(!below.valid);
  ASSERT_TRUE(!below.within_tolerance);
  ASSERT_EQ(below.min_equal_share_bps, 9499u);
  ASSERT_EQ(below.max_equal_share_bps, 10167u);

  auto above = native_load::summarize_canonical_lane_balance(
      2, 40000, {10501, 9833, 9833, 9833});
  ASSERT_TRUE(!above.valid);
  ASSERT_TRUE(!above.within_tolerance);
  ASSERT_EQ(above.min_equal_share_bps, 9833u);
  ASSERT_EQ(above.max_equal_share_bps, 10501u);
}

TEST(NativeLoadGeneratorPolicy, CanonicalLaneBalanceDepthZeroNeverGates) {
  auto balance = native_load::summarize_canonical_lane_balance(0, 7, {3});

  ASSERT_TRUE(!balance.required);
  ASSERT_TRUE(!balance.valid);
  ASSERT_TRUE(balance.depth_valid);
  ASSERT_TRUE(!balance.topology_complete);
  ASSERT_TRUE(!balance.every_lane_active);
  ASSERT_TRUE(!balance.within_tolerance);
  ASSERT_EQ(balance.expected_lanes, 0u);
  ASSERT_EQ(balance.observed_lanes, 1u);
  ASSERT_TRUE(!balance.totals_reconcile);
}

TEST(NativeLoadGeneratorPolicy, CanonicalLaneBalanceDepthBoundsAreShiftSafe) {
  auto deepest = native_load::summarize_canonical_lane_balance(60, 0, {});
  ASSERT_TRUE(deepest.required);
  ASSERT_TRUE(deepest.depth_valid);
  ASSERT_TRUE(!deepest.valid);
  ASSERT_EQ(deepest.expected_lanes, std::uint64_t{1} << 60);
  ASSERT_EQ(deepest.observed_lanes, 0u);

  auto invalid = native_load::summarize_canonical_lane_balance(61, 0, {});
  ASSERT_TRUE(invalid.required);
  ASSERT_TRUE(!invalid.depth_valid);
  ASSERT_TRUE(!invalid.valid);
  ASSERT_EQ(invalid.expected_lanes, 0u);

  auto far_invalid = native_load::summarize_canonical_lane_balance(
      std::numeric_limits<std::uint32_t>::max(), 0, {});
  ASSERT_TRUE(!far_invalid.depth_valid);
  ASSERT_TRUE(!far_invalid.valid);
  ASSERT_EQ(far_invalid.expected_lanes, 0u);
}

TEST(NativeLoadGeneratorPolicy, CanonicalLaneBalanceUsesOverflowSafeIntegerMath) {
  constexpr auto max = std::numeric_limits<std::uint64_t>::max();
  auto balanced = native_load::summarize_canonical_lane_balance(
      1, max, {max / 2, max - max / 2});
  ASSERT_TRUE(balanced.valid);
  ASSERT_TRUE(balanced.totals_reconcile);
  ASSERT_TRUE(!balanced.measured_transfers_sum_overflow);
  ASSERT_EQ(balanced.measured_transfers_sum, max);
  ASSERT_TRUE(balanced.min_equal_share_bps >= 9500);
  ASSERT_TRUE(balanced.max_equal_share_bps <= 10500);

  auto overflowing_sum = native_load::summarize_canonical_lane_balance(1, max, {max, max});
  ASSERT_TRUE(!overflowing_sum.valid);
  ASSERT_TRUE(overflowing_sum.measured_transfers_sum_overflow);
  ASSERT_TRUE(!overflowing_sum.totals_reconcile);
  ASSERT_EQ(overflowing_sum.measured_transfers_sum, max);
}

TEST(NativeLoadGeneratorPolicy, CanonicalLaneBalanceIsOrderIndependent) {
  auto first = native_load::summarize_canonical_lane_balance(
      2, 40000, {9500, 10500, 10000, 10000});
  auto second = native_load::summarize_canonical_lane_balance(
      2, 40000, {10000, 9500, 10000, 10500});

  ASSERT_EQ(first.valid, second.valid);
  ASSERT_EQ(first.topology_complete, second.topology_complete);
  ASSERT_EQ(first.totals_reconcile, second.totals_reconcile);
  ASSERT_EQ(first.every_lane_active, second.every_lane_active);
  ASSERT_EQ(first.within_tolerance, second.within_tolerance);
  ASSERT_EQ(first.measured_transfers_sum, second.measured_transfers_sum);
  ASSERT_EQ(first.min_measured_transfers, second.min_measured_transfers);
  ASSERT_EQ(first.max_measured_transfers, second.max_measured_transfers);
  ASSERT_EQ(first.min_equal_share_bps, second.min_equal_share_bps);
  ASSERT_EQ(first.max_equal_share_bps, second.max_equal_share_bps);
}
