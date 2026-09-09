#include <array>
#include <string>

#include "td/utils/tests.h"
#include "validator/interfaces/validator-manager.h"

namespace {

using ExternalWaitStats = ton::validator::CollationStats::ExternalWaitStats;
using ExternalWaitKind = ExternalWaitStats::Kind;
using NativeDeferralCounters = ton::validator::CollationStats::NativeDeferralCounters;
using NativeDeferralReason = NativeDeferralCounters::Reason;
constexpr auto native_deferral_reason_count = static_cast<std::size_t>(NativeDeferralReason::count);

constexpr std::array<const char*, 22> external_wait_keys{
    "external_wait_round_live_s=",
    "external_wait_round_live_calls=",
    "external_wait_round_native_coalescing_s=",
    "external_wait_round_native_coalescing_calls=",
    "external_wait_generic_try_pop_s=",
    "external_wait_generic_try_pop_calls=",
    "external_wait_generic_sync_snapshot_s=",
    "external_wait_generic_sync_snapshot_calls=",
    "external_wait_native_probe_s=",
    "external_wait_native_probe_calls=",
    "external_wait_native_first_work_s=",
    "external_wait_native_first_work_calls=",
    "external_wait_native_fragment_refill_s=",
    "external_wait_native_fragment_refill_calls=",
    "external_wait_native_post_commit_idle_s=",
    "external_wait_native_post_commit_idle_calls=",
    "external_wait_native_producer_drain_s=",
    "external_wait_native_producer_drain_calls=",
    "external_wait_native_sync_snapshot_s=",
    "external_wait_native_sync_snapshot_calls=",
    "external_wait_accounted_s=",
    "external_wait_calls=",
};

constexpr std::array<const char*, native_deferral_reason_count> native_deferral_entry_keys{
    "native_deferral_intake_deadline_idle_entries=",
    "native_deferral_intake_deadline_fragment_entries=",
    "native_deferral_checkpoint_deadline_rollback_entries=",
    "native_deferral_checkpoint_hard_preflight_entries=",
    "native_deferral_checkpoint_size_preflight_entries=",
    "native_deferral_medium_timeout_entries=",
    "native_deferral_candidate_headroom_entries=",
    "native_deferral_candidate_size_guard_entries=",
    "native_deferral_protocol_account_capacity_entries=",
    "native_deferral_account_unavailable_entries=",
    "native_deferral_account_balance_unrepresentable_entries=",
    "native_deferral_state_invalid_fields_entries=",
    "native_deferral_state_invalid_signature_entries=",
    "native_deferral_state_nonce_mismatch_entries=",
    "native_deferral_state_nonce_overflow_entries=",
    "native_deferral_state_invalid_source_entries=",
    "native_deferral_state_invalid_destination_entries=",
    "native_deferral_state_insufficient_balance_entries=",
    "native_deferral_state_balance_overflow_entries=",
};

bool contains_exact_stat(const std::string& stats, const char* key, td::uint64 value) {
  auto token = std::string{" "} + key + std::to_string(value) + " ";
  return stats.find(token) != std::string::npos;
}

}  // namespace

TEST(CollatorExternalWaitStats, AccountsEveryKindExactlyOnce) {
  ExternalWaitStats stats;
  constexpr auto kind_count = static_cast<std::size_t>(ExternalWaitKind::count);
  for (std::size_t i = 0; i < kind_count; ++i) {
    auto kind = static_cast<ExternalWaitKind>(i);
    auto seconds = static_cast<double>(i + 1) / 8.0;
    stats.record(kind, seconds);
    ASSERT_EQ(stats.seconds(kind), seconds);
    ASSERT_EQ(stats.calls(kind), 1u);
  }
  ASSERT_EQ(stats.total_seconds(), 6.875);
  ASSERT_EQ(stats.total_calls(), kind_count);
}

TEST(CollatorExternalWaitStats, SerializesStableKeysForWallTimeOnly) {
  ton::validator::CollationStats stats;
  stats.external_wait.record(ExternalWaitKind::generic_sync_snapshot, 0.125);

  auto real_stats = stats.work_time_to_str(false);
  auto cpu_stats = stats.work_time_to_str(true);
  for (auto key : external_wait_keys) {
    ASSERT_TRUE(real_stats.find(key) != std::string::npos);
  }
  ASSERT_TRUE(real_stats.find("external_wait_accounted_s=0.125") != std::string::npos);
  ASSERT_TRUE(real_stats.find("external_wait_calls=1") != std::string::npos);
  ASSERT_TRUE(cpu_stats.find("external_wait_") == std::string::npos);
}

TEST(CollatorExternalWaitStats, DeliveryWaitOutcomesPartitionEachProducerState) {
  using DeliveryStats = ton::validator::CollationStats::NativeDeliveryStats;
  using Outcome = DeliveryStats::Outcome;
  DeliveryStats stats;
  for (auto kind : {ExternalWaitKind::native_first_work, ExternalWaitKind::native_fragment_refill,
                    ExternalWaitKind::native_post_commit_idle}) {
    for (bool pending : {false, true}) {
      stats.record_wait(kind, pending, 0.125, Outcome::work);
      stats.record_wait(kind, pending, 0.25, Outcome::marker);
      stats.record_wait(kind, pending, 0.375, Outcome::timeout);
      stats.record_wait(kind, pending, 0.5, Outcome::error);
      const auto& bucket = stats.wait(kind, pending);
      ASSERT_EQ(bucket.seconds, 1.25);
      ASSERT_EQ(bucket.calls, 4u);
      ASSERT_EQ(bucket.work_wakes, 1u);
      ASSERT_EQ(bucket.marker_wakes, 1u);
      ASSERT_EQ(bucket.timeouts, 1u);
      ASSERT_EQ(bucket.errors, 1u);
      ASSERT_EQ(bucket.calls, bucket.work_wakes + bucket.marker_wakes + bucket.timeouts + bucket.errors);
    }
  }
}

TEST(CollatorExternalWaitStats, DeliveryTimersDoNotInflateLegacyWaitOrCpuAccounting) {
  using Outcome = ton::validator::CollationStats::NativeDeliveryStats::Outcome;
  ton::validator::CollationStats stats;
  stats.external_wait.record(ExternalWaitKind::native_first_work, 0.5);
  stats.native_delivery.record_probe(true, 0.125, true);
  stats.native_delivery.record_probe(false, 0.0625, false);
  stats.native_delivery.record_wait(ExternalWaitKind::native_first_work, true, 0.25, Outcome::work);
  ASSERT_EQ(stats.external_wait.total_seconds(), 0.5);
  ASSERT_EQ(stats.native_delivery.probe(true).seconds, 0.125);
  ASSERT_EQ(stats.native_delivery.probe(true).work_wakes, 1u);
  ASSERT_EQ(stats.native_delivery.probe(false).calls, 1u);
  ASSERT_EQ(stats.native_delivery.probe(false).work_wakes, 0u);
  const auto wall = stats.work_time_to_str(false);
  ASSERT_TRUE(wall.find("external_delivery_first_work_producer_pending_s=0.25") != std::string::npos);
  ASSERT_TRUE(wall.find("external_delivery_first_work_producer_pending_work_wakes=1") != std::string::npos);
  ASSERT_TRUE(wall.find("external_delivery_probe_published_s=0.125") != std::string::npos);
  ASSERT_TRUE(wall.find("external_delivery_probe_unconfirmed_s=0.0625") != std::string::npos);
  ASSERT_TRUE(wall.find("external_wait_accounted_s=0.5") != std::string::npos);
  ASSERT_TRUE(stats.work_time_to_str(true).find("external_delivery_") == std::string::npos);
}

TEST(CollatorExternalWaitStats, PublishedLowerBoundDoesNotCountReservedOrAlreadyConsumedWork) {
  ton::validator::ExtMsgQueueState state;
  state.record_selected(8, 128);
  state.record_push_started(4, 64);
  // The producer may be suspended in a bounded push. The consumer can observe
  // some inserted entries before that push has returned its completed count.
  ASSERT_EQ(state.native_published_ahead_lower_bound(), 0u);
  state.record_consumed(2, 32);
  ASSERT_EQ(state.native_published_ahead_lower_bound(), 0u);
  state.record_push_completed(4, 4, 64, 64);
  ASSERT_EQ(state.native_published_ahead_lower_bound(), 2u);
  state.record_push_started(4, 64);
  // New reserved work must not inflate the known-published prefix.
  ASSERT_EQ(state.native_published_ahead_lower_bound(), 2u);
  state.record_consumed(3, 48);
  ASSERT_EQ(state.native_published_ahead_lower_bound(), 0u);
  state.record_push_completed(4, 4, 64, 64);
  ASSERT_EQ(state.native_published_ahead_lower_bound(), 3u);
  state.record_consumed(3, 48);
  ASSERT_EQ(state.native_published_ahead_lower_bound(), 0u);
}

TEST(CollatorExternalWaitStats, SerializesNativeCheckpointCoalescingTelemetry) {
  ton::validator::CollationStats stats;
  stats.native_checkpoint_groups = 2;
  stats.native_checkpoint_group_max_entries = 2'048;
  stats.native_checkpoint_group_max_fragments = 4;
  stats.native_checkpoint_flush_ingress = 1;
  stats.native_checkpoint_flush_latency = 2;
  stats.native_checkpoint_refill_continuations = 3;
  stats.native_checkpoint_refill_expirations = 1;
  stats.native_checkpoint_ingress_retentions = 4;
  stats.native_checkpoint_ingress_retention_max_dirty_accounts = 1'536;
  stats.native_checkpoint_rollbacks = 1;
  stats.native_checkpoint_rollback_entries = 512;

  auto real_stats = stats.work_time_to_str(false);
  ASSERT_TRUE(real_stats.find("native_checkpoint_groups=2") != std::string::npos);
  ASSERT_TRUE(real_stats.find("native_checkpoint_group_max_entries=2048") != std::string::npos);
  ASSERT_TRUE(real_stats.find("native_checkpoint_group_max_fragments=4") != std::string::npos);
  ASSERT_TRUE(real_stats.find("native_checkpoint_flush_ingress=1") != std::string::npos);
  ASSERT_TRUE(real_stats.find("native_checkpoint_flush_latency=2") != std::string::npos);
  ASSERT_TRUE(real_stats.find("native_checkpoint_refill_continuations=3") != std::string::npos);
  ASSERT_TRUE(real_stats.find("native_checkpoint_refill_expirations=1") != std::string::npos);
  ASSERT_TRUE(real_stats.find("native_checkpoint_ingress_retentions=4") != std::string::npos);
  ASSERT_TRUE(real_stats.find("native_checkpoint_ingress_retention_max_dirty_accounts=1536") != std::string::npos);
  ASSERT_TRUE(real_stats.find("native_checkpoint_rollbacks=1") != std::string::npos);
  ASSERT_TRUE(real_stats.find("native_checkpoint_rollback_entries=512") != std::string::npos);
}

TEST(CollatorExternalWaitStats, SerializesRegisteredNativeRunReuseTelemetry) {
  ton::validator::CollationStats stats;
  stats.native_registered_run_reuses = 123;

  auto real_stats = stats.work_time_to_str(false);
  auto cpu_stats = stats.work_time_to_str(true);
  ASSERT_TRUE(contains_exact_stat(real_stats, "native_registered_run_reuses=", 123));
  ASSERT_TRUE(contains_exact_stat(cpu_stats, "native_registered_run_reuses=", 123));
}

TEST(CollatorExternalWaitStats, AccountsAndSerializesNativeDeferrals) {
  NativeDeferralCounters counters;
  for (std::size_t i = 0; i < native_deferral_reason_count; ++i) {
    counters.add(static_cast<NativeDeferralReason>(i), i + 1);
  }
  ASSERT_EQ(counters.total_entries(), 190u);
  counters.record_protocol_capacity_requeue(16);
  counters.record_carryover_requeue(8);
  counters.record_scalar_decode_retry();

  NativeDeferralCounters additional;
  for (std::size_t i = 0; i < native_deferral_reason_count; ++i) {
    additional.add(static_cast<NativeDeferralReason>(i), (i + 1) * 10);
  }
  additional.record_protocol_capacity_requeue(4);
  additional.record_carryover_requeue(3);
  additional.record_scalar_decode_retry();
  counters.merge(additional);

  ASSERT_EQ(counters.total_entries(), 2'090u);
  for (std::size_t i = 0; i < native_deferral_reason_count; ++i) {
    ASSERT_EQ(counters.entries(static_cast<NativeDeferralReason>(i)), (i + 1) * 11);
  }
  ASSERT_EQ(counters.prebatch_protocol_capacity_requeue_works, 2u);
  ASSERT_EQ(counters.prebatch_protocol_capacity_requeue_entries, 20u);
  ASSERT_EQ(counters.prebatch_carryover_requeue_works, 2u);
  ASSERT_EQ(counters.prebatch_carryover_requeue_entries, 11u);
  ASSERT_EQ(counters.prebatch_scalar_decode_retry_works, 2u);

  ton::validator::CollationStats stats;
  stats.native_deferrals = counters;
  stats.native_microbatch_delayed = counters.total_entries();
  auto real_stats = stats.work_time_to_str(false);
  ASSERT_TRUE(contains_exact_stat(real_stats, "native_microbatch_delayed=", counters.total_entries()));
  for (std::size_t i = 0; i < native_deferral_reason_count; ++i) {
    ASSERT_TRUE(contains_exact_stat(real_stats, native_deferral_entry_keys[i], (i + 1) * 11));
  }
  ASSERT_TRUE(contains_exact_stat(real_stats, "native_prebatch_protocol_capacity_requeue_works=", 2));
  ASSERT_TRUE(contains_exact_stat(real_stats, "native_prebatch_protocol_capacity_requeue_entries=", 20));
  ASSERT_TRUE(contains_exact_stat(real_stats, "native_prebatch_carryover_requeue_works=", 2));
  ASSERT_TRUE(contains_exact_stat(real_stats, "native_prebatch_carryover_requeue_entries=", 11));
  ASSERT_TRUE(contains_exact_stat(real_stats, "native_prebatch_scalar_decode_retry_works=", 2));
}

TEST(CollatorExternalWaitStats, NativeAccountHistogramsPreserveThresholdBoundaries) {
  ton::validator::CollationStats stats;
  // Both edges of each disjoint bucket, including empty/rejected fragments.
  for (auto accounts : {0u, 64u, 65u, 80u, 81u, 128u, 129u, 256u, 257u, 511u, 512u, 513u, 4096u}) {
    stats.native_microbatch_account_histogram.record(accounts);
    stats.record_native_staged_updates(accounts, 1);
  }
  constexpr std::array<td::uint64, 7> expected{2, 2, 2, 2, 2, 1, 2};
  for (bool cpu : {false, true}) {
    const auto serialized = stats.work_time_to_str(cpu);
    for (std::size_t i = 0; i < expected.size(); ++i) {
      auto suffix = stats.native_microbatch_account_histogram.suffixes[i];
      ASSERT_EQ(stats.native_microbatch_account_histogram.buckets[i], expected[i]);
      ASSERT_TRUE(contains_exact_stat(serialized, (std::string{"native_microbatch_accounts_"} + suffix + "=").c_str(), expected[i]));
      ASSERT_TRUE(contains_exact_stat(serialized, (std::string{"native_staged_updates_"} + suffix + "=").c_str(), expected[i]));
    }
    ASSERT_TRUE(contains_exact_stat(serialized, "native_staged_workers_1=", 13));
    ASSERT_TRUE(contains_exact_stat(serialized, "native_staged_workers_other=", 0));
  }
  stats.record_native_staged_updates(512, 2);
  stats.record_native_staged_updates(512, 4);
  stats.record_native_staged_updates(512, 8);
  stats.record_native_staged_updates(512, 6);
  for (std::size_t i = 1; i < 5; ++i) {
    ASSERT_EQ(stats.native_staged_worker_histogram[i], 1u);
  }
}
