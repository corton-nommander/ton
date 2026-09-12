#include <numeric>

#include "td/actor/BusRuntime.h"
#include "td/utils/tests.h"
#include "validator/native-lane-scheduler.h"

namespace ton::validator {
namespace {

std::vector<ShardIdFull> four_lanes() {
  return {{basechainId, 0x2000000000000000ull}, {basechainId, 0x6000000000000000ull},
          {basechainId, 0xa000000000000000ull}, {basechainId, 0xe000000000000000ull}};
}

struct ProbeResult {
  std::atomic<unsigned> remaining{30};
  std::atomic<unsigned> checked{0};
  void complete(unsigned expected) {
    CHECK(td::actor::SchedulerContext::get().get_scheduler_id().value() == expected);
    checked.fetch_add(1, std::memory_order_relaxed);
    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      td::actor::SchedulerContext::get().stop();
    }
  }
};

class ChildProbe final : public td::actor::Actor {
 public:
  ChildProbe(std::shared_ptr<ProbeResult> result, unsigned expected)
      : result_(std::move(result)), expected_(expected) {
  }
  void start_up() override {
    result_->complete(expected_);
    stop();
  }
 private:
  std::shared_ptr<ProbeResult> result_;
  unsigned expected_;
};

struct ProbeBus final : td::actor::Bus {
  using Events = td::TypeList<>;
  std::shared_ptr<ProbeResult> result;
  unsigned expected{0};
};

class BusProbe final : public td::actor::SpawnsWith<ProbeBus>, public td::actor::ConnectsTo<> {
 public:
  void start_up() override {
    owning_bus()->result->complete(owning_bus()->expected);
    stop();
  }
};

class BridgeProbe final : public td::actor::Actor {
 public:
  BridgeProbe(std::shared_ptr<ProbeResult> result, ShardIdFull shard, unsigned expected)
      : result_(std::move(result)), shard_(shard), expected_(expected) {
  }
  void start_up() override {
    // The production Bridge creates its facade and BusRuntime here. Verify
    // both inherit the selected context without requiring a BusRuntime change.
    td::actor::create_actor<ChildProbe>("facade-probe", result_, expected_).release();
    td::actor::Runtime runtime;
    runtime.register_actor<BusProbe>("bus-probe");
    auto bus = std::make_shared<ProbeBus>();
    bus->result = result_;
    bus->expected = expected_;
    runtime.start(std::move(bus));
    for (auto kind : {NativeLaneActorKind::collator, NativeLaneActorKind::validator, NativeLaneActorKind::producer}) {
      td::actor::create_actor<ChildProbe>(native_lane_actor_options("explicit-probe", shard_, kind),
                                          result_, expected_).release();
    }
    result_->complete(expected_);
    stop();
  }
 private:
  std::shared_ptr<ProbeResult> result_;
  ShardIdFull shard_;
  unsigned expected_;
};

}  // namespace

TEST(NativeLaneScheduler, DisabledPreservesAllConfiguredWorkers) {
  for (unsigned threads = 1; threads <= 127; ++threads) {
    auto plan = NativeLaneSchedulerPlan::make(threads, 0).move_as_ok();
    ASSERT_EQ(plan.domains, 0u);
    ASSERT_EQ(plan.cpu_threads[0], threads);
    ASSERT_EQ(std::accumulate(plan.cpu_threads.begin(), plan.cpu_threads.end(), 0u), threads);
    for (const auto &shard : four_lanes()) { ASSERT_EQ(plan.scheduler_for(shard, true), 0u); }
  }
}

TEST(NativeLaneScheduler, EnabledConservesWorkersIncludingUnevenRemainders) {
  ASSERT_TRUE(NativeLaneSchedulerPlan::make(0, 0).is_error());
  ASSERT_TRUE(NativeLaneSchedulerPlan::make(16, 2).is_error());
  ASSERT_TRUE(NativeLaneSchedulerPlan::make(7, 4).is_error());
  for (unsigned threads = 8; threads <= 127; ++threads) {
    auto plan = NativeLaneSchedulerPlan::make(threads, 4).move_as_ok();
    ASSERT_EQ(std::accumulate(plan.cpu_threads.begin(), plan.cpu_threads.end(), 0u), threads);
    ASSERT_EQ(plan.cpu_threads[0], 4u);
    unsigned smallest = plan.cpu_threads[4];
    unsigned largest = plan.cpu_threads[1];
    ASSERT_TRUE(smallest >= 1 && largest <= smallest + 1);
  }
  auto plan = NativeLaneSchedulerPlan::make(16, 4).move_as_ok();
  ASSERT_EQ(plan.cpu_threads, (std::array<unsigned, 5>{4, 3, 3, 3, 3}));
}

TEST(NativeLaneScheduler, OnlyCompleteExactFourLeafTopologyCanRoute) {
  auto plan = NativeLaneSchedulerPlan::make(16, 4).move_as_ok();
  const auto lanes = four_lanes();
  ASSERT_TRUE(NativeLaneSchedulerPlan::supports(lanes));
  for (unsigned i = 0; i < lanes.size(); ++i) {
    ASSERT_EQ(plan.scheduler_for(lanes[i], true), i + 1);
    ASSERT_EQ(plan.scheduler_for(lanes[i], false), 0u);
  }
  ASSERT_EQ(plan.scheduler_for({masterchainId, shardIdAll}, true), 0u);
  ASSERT_EQ(plan.scheduler_for({basechainId, shardIdAll}, true), 0u);
  ASSERT_EQ(plan.scheduler_for({basechainId, 0x1000000000000000ull}, true), 0u);
  ASSERT_EQ(plan.scheduler_for({1, 0x2000000000000000ull}, true), 0u);
  auto missing = lanes;
  missing.pop_back();
  ASSERT_TRUE(!NativeLaneSchedulerPlan::supports(missing));
  auto duplicate = lanes;
  duplicate.back() = duplicate.front();
  ASSERT_TRUE(!NativeLaneSchedulerPlan::supports(duplicate));
  auto split = lanes;
  split.erase(split.begin());
  split.push_back({basechainId, 0x1000000000000000ull});
  split.push_back({basechainId, 0x3000000000000000ull});
  ASSERT_TRUE(!NativeLaneSchedulerPlan::supports(split));
  auto extra_nonbase = lanes;
  extra_nonbase.push_back({masterchainId, shardIdAll});
  ASSERT_TRUE(NativeLaneSchedulerPlan::supports(extra_nonbase));
}

TEST(NativeLaneScheduler, RealActorAndBusChildrenExecuteInAssignedDomain) {
  auto &runtime = native_lane_scheduler_runtime();
  runtime.plan = NativeLaneSchedulerPlan::make(8, 4).move_as_ok();
  publish_native_lane_scheduler_topology(four_lanes());
  std::vector<td::actor::Scheduler::NodeInfo> groups;
  for (auto count : runtime.plan.cpu_threads) { groups.emplace_back(count); }
  auto result = std::make_shared<ProbeResult>();
  {
    td::actor::Scheduler scheduler(std::move(groups));
    scheduler.run_in_context([&] {
      auto shards = four_lanes();
      shards.push_back({masterchainId, shardIdAll});
      for (unsigned i = 0; i < shards.size(); ++i) {
        const auto expected = i < 4 ? i + 1 : 0;
        td::actor::create_actor<BridgeProbe>(native_lane_actor_options("bridge-probe", shards[i],
                                                                       NativeLaneActorKind::bridge),
                                              result, shards[i], expected).release();
      }
    });
    scheduler.run();
  }
  ASSERT_EQ(result->checked.load(std::memory_order_relaxed), 30u);
  const auto stats = native_lane_scheduler_stats();
  ASSERT_TRUE(stats.find("cpu_threads:8") != std::string::npos);
  ASSERT_TRUE(stats.find("bridge_group4:1") != std::string::npos);
  ASSERT_TRUE(stats.find("producer_group4:1") != std::string::npos);
  // Unsupported transitions affect new placement; existing actor placement is
  // immutable and follows ordinary session retirement, not unsafe migration.
  publish_native_lane_scheduler_topology({{basechainId, shardIdAll}});
  ASSERT_TRUE(!runtime.topology_supported.load(std::memory_order_acquire));
  runtime.plan = NativeLaneSchedulerPlan::make(8, 0).move_as_ok();
}

}  // namespace ton::validator
