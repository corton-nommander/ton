/*
    This file is part of TON Blockchain Library.
    Licensed under the GNU Lesser General Public License, version 2 or later.
*/
#pragma once

#include <array>
#include <atomic>
#include <vector>

#include "td/actor/actor.h"
#include "td/utils/Status.h"
#include "td/utils/StringBuilder.h"
#include "ton/ton-shard.h"

namespace ton::validator {

// CPU worker counts, not CPU affinity or independent validator committees.
// Each TD scheduler group also has its existing I/O/event-loop infrastructure.
struct NativeLaneSchedulerPlan {
  unsigned domains{0};
  unsigned total_cpu_threads{0};
  std::array<unsigned, 5> cpu_threads{};

  static td::Result<NativeLaneSchedulerPlan> make(unsigned total, unsigned lanes) {
    if (total == 0 || (lanes != 0 && lanes != 4)) {
      return td::Status::Error("TON_NATIVE_LANE_SCHEDULERS must be 0 or 4 with a positive engine thread count");
    }
    NativeLaneSchedulerPlan plan;
    plan.domains = lanes;
    plan.total_cpu_threads = total;
    if (lanes == 0) {
      plan.cpu_threads[0] = total;
      return plan;
    }
    if (total < 8) {
      return td::Status::Error("four native lane schedulers require at least 8 configured engine CPU threads");
    }
    plan.cpu_threads[0] = 4;
    const auto remaining = total - plan.cpu_threads[0];
    for (unsigned lane = 0; lane < 4; ++lane) {
      plan.cpu_threads[lane + 1] = remaining / 4 + (lane < remaining % 4);
    }
    return plan;
  }

  // Only the complete four-leaf basechain topology enables routing. Neither
  // masterchain, a wider callback nor a split/merged leaf inherits a lane ID.
  static bool supports(const std::vector<ShardIdFull> &shards) {
    unsigned seen = 0;
    for (const auto &shard : shards) {
      if (shard.workchain != basechainId) { continue; }
      if (shard_pfx_len(shard.shard) != 2) { return false; }
      const unsigned lane = static_cast<unsigned>(shard.shard >> 62);
      const unsigned bit = 1u << lane;
      if (seen & bit) { return false; }
      seen |= bit;
    }
    return seen == 15;
  }

  unsigned scheduler_for(ShardIdFull shard, bool topology_supported) const {
    if (domains != 4 || !topology_supported || shard.workchain != basechainId ||
        shard_pfx_len(shard.shard) != 2) {
      return 0;
    }
    return 1 + static_cast<unsigned>(shard.shard >> 62);
  }
};

enum class NativeLaneActorKind : unsigned { bridge, collator, validator, producer, admission, count };

struct NativeLaneSchedulerRuntime {
  // Written only by validator-engine main before starting any actors.
  NativeLaneSchedulerPlan plan;
  // The manager publishes topology; these facts do not authorize account work.
  std::atomic<bool> topology_supported{false};
  std::atomic<td::uint64> topology_updates{0}, unsupported_updates{0};
  std::array<std::array<std::atomic<td::uint64>, 5>, static_cast<unsigned>(NativeLaneActorKind::count)> placements{};
};

inline NativeLaneSchedulerRuntime &native_lane_scheduler_runtime() {
  static NativeLaneSchedulerRuntime runtime;
  return runtime;
}

inline void publish_native_lane_scheduler_topology(const std::vector<ShardIdFull> &shards) {
  auto &runtime = native_lane_scheduler_runtime();
  if (runtime.plan.domains == 0) { return; }
  const bool supported = NativeLaneSchedulerPlan::supports(shards);
  runtime.topology_supported.store(supported, std::memory_order_release);
  runtime.topology_updates.fetch_add(1, std::memory_order_relaxed);
  if (!supported) { runtime.unsupported_updates.fetch_add(1, std::memory_order_relaxed); }
}

inline td::actor::ActorOptions native_lane_actor_options(td::Slice name, ShardIdFull shard, NativeLaneActorKind kind) {
  auto options = td::actor::ActorOptions{}.with_name(name);
  auto &runtime = native_lane_scheduler_runtime();
  if (runtime.plan.domains == 0) { return options; }
  const auto scheduler = runtime.plan.scheduler_for(shard, runtime.topology_supported.load(std::memory_order_acquire));
  runtime.placements[static_cast<unsigned>(kind)][scheduler].fetch_add(1, std::memory_order_relaxed);
  return options.on_scheduler(td::actor::SchedulerId{static_cast<td::uint8>(scheduler)});
}

inline std::string native_lane_scheduler_stats() {
  auto &runtime = native_lane_scheduler_runtime();
  td::StringBuilder out;
  out << "enabled:" << (runtime.plan.domains != 0) << " domains:" << runtime.plan.domains
      << " cpu_threads:" << runtime.plan.total_cpu_threads << " main_cpu_threads:" << runtime.plan.cpu_threads[0]
      << " topology_supported:" << runtime.topology_supported.load(std::memory_order_acquire)
      << " topology_updates:" << runtime.topology_updates.load(std::memory_order_relaxed)
      << " unsupported_updates:" << runtime.unsupported_updates.load(std::memory_order_relaxed);
  constexpr const char *names[] = {"bridge", "collator", "validator", "producer", "admission"};
  for (unsigned scheduler = 0; scheduler <= runtime.plan.domains; ++scheduler) {
    out << " group" << scheduler << "_cpu_threads:" << runtime.plan.cpu_threads[scheduler];
    for (unsigned kind = 0; kind < static_cast<unsigned>(NativeLaneActorKind::count); ++kind) {
      out << ' ' << names[kind] << "_group" << scheduler << ':'
          << runtime.placements[kind][scheduler].load(std::memory_order_relaxed);
    }
  }
  return out.as_cslice().str();
}

}  // namespace ton::validator
