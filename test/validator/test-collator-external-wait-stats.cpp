#include <array>
#include <string>

#include "td/utils/tests.h"
#include "validator/interfaces/validator-manager.h"

namespace {

using ExternalWaitStats = ton::validator::CollationStats::ExternalWaitStats;
using ExternalWaitKind = ExternalWaitStats::Kind;

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
