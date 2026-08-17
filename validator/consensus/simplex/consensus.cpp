/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/simplex/state.h"
#include "consensus/stats.h"
#include "consensus/utils.h"
#include "td/actor/coro_utils.h"

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace {

struct SlotState {
  SlotState(td::Unit) {
  }

  std::optional<CandidateRef> pending_block;
  std::optional<CandidateId> voted_notar;
  std::optional<CandidateId> notar_cert;
  bool voted_skip = false;
  bool voted_final = false;
};

class ConsensusImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
  using State = ConsensusState<SlotState, td::Unit>;

 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    auto [awaiter, promise] = td::actor::StartedTask<StartEvent>::make_bridge();

    auto& bus = *owning_bus();

    slots_per_leader_window_ = bus.config.slots_per_leader_window;
    params_ = bus.config.noncritical_params;
    first_block_timeout_ = params_.first_block_timeout;
    if (max_tps_mode_enabled()) {
      LOG(WARNING) << "Simplex work-driven candidate failure timeout: " << max_tps_candidate_timeout().count()
                   << "ms, local work budget: " << max_tps_candidate_work_timeout().count()
                   << "ms (successful blocks are not paced by either timeout)";
    }
    state_.emplace(State({}));

    for (const auto& vote : bus.bootstrap_votes) {
      auto slot = state_->slot_at(vote.referenced_slot());
      if (!slot.has_value()) {
        continue;
      }

      auto notar_fn = [&](const NotarizeVote& notar_vote) {
        if (slot->state->voted_notar.has_value() && slot->state->voted_notar != notar_vote.id) {
          // Note that a bug might have caused conflicting votes to be casted. In this case, Pool
          // will crash but not before database durably stores the votes, so in bootstrap_votes we
          // might observe conflicts. In such a case, at least let's not corrupt local per-slot
          // invariants.
          LOG(WARNING) << "Dropping corrupted " << notar_vote;
          return;
        }
        slot->state->voted_notar = notar_vote.id;
      };
      auto final_fn = [&](const FinalizeVote& final_vote) {
        if (slot->state->voted_skip ||
            (slot->state->voted_notar.has_value() && slot->state->voted_notar != final_vote.id)) {
          LOG(WARNING) << "Dropping corrupted " << final_vote;
          return;
        }
        slot->state->voted_final = true;
      };
      auto skip_fn = [&](const SkipVote& skip_vote) {
        if (slot->state->voted_final) {
          LOG(WARNING) << "Dropping corrupted " << skip_vote;
          return;
        }
        slot->state->voted_skip = true;
      };
      std::visit(td::overloaded(notar_fn, final_fn, skip_fn), vote.vote);
    }

    if (auto window = bus.first_nonannounced_window) {
      auto start_slot = (window - 1) * slots_per_leader_window_;
      auto end_slot = window * slots_per_leader_window_;
      for (td::uint32 i = start_slot; i < end_slot; ++i) {
        auto slot = state_->slot_at(i);
        if (slot.has_value() && !slot->state->voted_final) {
          slot->state->voted_skip = true;
          owning_bus().publish<BroadcastVote>(SkipVote{i}).start().detach();
        }
      }
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const NoncriticalParamsUpdated> event) {
    params_ = event->params;
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const FinalizationObserved> event) {
    state_->notify_finalized(event->id.slot);
  }

  template <>
  void handle(BusHandle bus, std::shared_ptr<const NotarizationObserved> event) {
    process_notarization_observed(bus, event).start().detach();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const LeaderWindowObserved> event) {
    auto& bus = *owning_bus();
    if (event->start_slot < first_active_slot_) {
      LOG(WARNING) << "Ignoring stale leader-window observation at slot " << event->start_slot
                   << "; first active slot is " << first_active_slot_;
      return;
    }
    first_active_slot_ = event->start_slot;
    ++generation_epoch_;
    owning_bus().publish<ConsensusSlotAdvanced>(event->start_slot);
    td::uint32 new_window = event->start_slot / slots_per_leader_window_;
    current_window_ = new_window;

    if (previous_window_had_skip_) {
      first_block_timeout_ = std::min<std::chrono::duration<double>>(
          first_block_timeout_ * params_.first_block_timeout_multiplier, params_.first_block_timeout_cap);
    } else {
      first_block_timeout_ = params_.first_block_timeout;
    }

    td::uint32 offset = event->start_slot % slots_per_leader_window_;
    if (offset == 0) {
      previous_window_had_skip_ = false;

      if (bus.collator_schedule->is_expected_collator(bus.local_id.idx, event->start_slot)) {
        start_generation(event->base, event->start_slot, generation_epoch_).start().detach();
      }
    }

    if (timeout_slot_ <= event->start_slot) {
      timeout_slot_ = event->start_slot + 1;
      if (max_tps_mode_enabled()) {
        timeout_base_ = td::Timestamp::now();
        alarm_timestamp() = td::Timestamp::in(max_tps_candidate_timeout(), timeout_base_);
      } else {
        timeout_base_ = td::Timestamp::in(std::chrono::round<std::chrono::nanoseconds>(first_block_timeout_));
        alarm_timestamp() = td::Timestamp::in(params_.target_rate, timeout_base_);
      }
    }
  }

  void alarm() override {
    td::uint32 range_start = timeout_slot_ - 1;
    td::uint32 window_start = range_start - range_start % slots_per_leader_window_;
    td::uint32 window_end = window_start + slots_per_leader_window_;
    for (td::uint32 i = range_start; i < window_end; ++i) {
      auto slot = state_->slot_at(i);
      if (slot && !slot->state->voted_final) {
        owning_bus().publish<BroadcastVote>(SkipVote{i}).start().detach();
        slot->state->voted_skip = true;
        previous_window_had_skip_ = true;
      }
    }
    first_active_slot_ = std::max(first_active_slot_, window_end);
    current_window_ = std::max(current_window_, window_end / slots_per_leader_window_);
    ++generation_epoch_;
    owning_bus().publish<ConsensusSlotAdvanced>(window_end);
    timeout_slot_ = window_end;
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateReceived> event) {
    td::uint32 slot_idx = event->candidate->id.slot;
    td::uint32 first_too_new_slot = (current_window_ + params_.max_leader_window_desync + 1) * slots_per_leader_window_;
    if (slot_idx >= first_too_new_slot) {
      LOG(WARNING) << "Dropping too new candidate from " << event->candidate->leader << " : slot=" << slot_idx
                   << ", current_window=" << current_window_ * slots_per_leader_window_;
      return;
    }
    auto slot = state_->slot_at(slot_idx);
    if (!slot.has_value()) {
      return;
    }
    if (slot->state->voted_notar) {
      return;
    }
    const auto& candidate = event->candidate;

    if (candidate->parent_id.has_value() && candidate->parent_id->slot >= candidate->id.slot) {
      // FIXME: report misbehavior
      return;
    }

    if (slot->state->pending_block.has_value()) {
      if (slot->state->pending_block.value()->id != candidate->id) {
        // FIXME: Report misbehavior
      }
      return;
    }

    slot->state->pending_block = candidate;
    if (candidate->leader != owning_bus()->local_id.idx) {
      owning_bus().publish<TraceEvent>(stats::CandidateReceived::create(candidate, false));
    }
    try_notarize(*slot).start().detach();
  }

 private:
  td::actor::Task<> start_generation(ParentId base, td::uint32 start_slot, td::uint64 generation_epoch) {
    auto parent = co_await owning_bus().publish<ResolveState>(base);
    td::Timestamp start_time = td::Timestamp::now();
    if (!max_tps_mode_enabled() && parent.gen_utime_exact.has_value()) {
      start_time = std::max(start_time, td::Timestamp::at_unix(*parent.gen_utime_exact) + params_.target_rate);
      start_time = std::min(start_time, td::Timestamp::in(params_.target_rate));
    }

    // ResolveState may suspend past an alarm/skip into a newer window.  Both
    // the epoch and slot watermark are checked after that await so an old
    // resolution cannot resurrect local production for an obsolete window.
    if (generation_epoch != generation_epoch_ || start_slot < first_active_slot_ ||
        current_window_ != start_slot / slots_per_leader_window_) {
      LOG(WARNING) << "Dropping stale local generation start at slot " << start_slot
                   << "; first_active_slot=" << first_active_slot_
                   << " generation_epoch=" << generation_epoch << " current_epoch=" << generation_epoch_;
      co_return {};
    }

    if (max_tps_mode_enabled()) {
      // ResolveState has its own failure coverage from LeaderWindowObserved.
      // Once it succeeds, align the protocol alarm with the producer/collator
      // budget so a valid large candidate gets the complete configured time.
      timeout_base_ = td::Timestamp::now();
      alarm_timestamp() = td::Timestamp::in(max_tps_candidate_timeout(), timeout_base_);
    }

    owning_bus().publish<OurLeaderWindowStarted>(base, parent.state, start_slot, start_slot + slots_per_leader_window_,
                                                 start_time, std::move(parent.excluded_ext_messages));
    co_return {};
  }

  td::actor::Task<> try_notarize(State::SlotRef slot) {
    const auto& candidate = *slot.state->pending_block;
    auto is_voteable = [&] {
      auto current = state_->slot_at(slot.i);
      return current.has_value() && current->state == slot.state &&
             slot.i / slots_per_leader_window_ >= current_window_ && !slot.state->voted_final &&
             !slot.state->voted_notar && slot.state->pending_block.has_value() &&
             (*slot.state->pending_block)->id == candidate->id;
    };
    auto abandon_obsolete = [&] {
      if (is_voteable()) {
        return false;
      }
      LOG(WARNING) << "Abandoning notarization of obsolete or already-voted candidate " << candidate->id
                   << ": skipped=" << slot.state->voted_skip
                   << " voted_notar=" << slot.state->voted_notar.has_value()
                   << " voted_final=" << slot.state->voted_final;
      return true;
    };
    auto store_candidate = owning_bus().publish<StoreCandidate>(candidate).start();

    auto maybe_misbehavior = co_await owning_bus().publish<WaitForParent>(candidate);
    if (maybe_misbehavior) {
      owning_bus().publish<MisbehaviorReport>(candidate->leader, *maybe_misbehavior);
      co_return {};
    }
    if (abandon_obsolete()) {
      co_return {};
    }

    auto parent = co_await owning_bus().publish<ResolveState>(candidate->parent_id);
    if (abandon_obsolete()) {
      co_return {};
    }

    if (!max_tps_mode_enabled() && !candidate->is_empty() && parent.gen_utime_exact.has_value()) {
      auto earliest = td::Timestamp::at_unix(*parent.gen_utime_exact) + params_.min_block_interval;
      if (!earliest.is_in_past()) {
        co_await td::actor::coro_sleep(earliest);
      }
    }
    if (abandon_obsolete()) {
      co_return {};
    }

    auto validation_result = co_await owning_bus().publish<ValidationRequest>(parent.state, candidate);

    if (validation_result.has<CandidateReject>()) {
      LOG(WARNING) << "Candidate " << candidate->id
                   << " is rejected: " << validation_result.get<CandidateReject>().reason;
      // FIXME: Report misbehavior
      co_return {};
    }
    if (abandon_obsolete()) {
      co_return {};
    }
    co_await std::move(store_candidate);

    // Every await above can yield while the slot is finalized, replaced, or a
    // newer leader window becomes active. This final check and vote-state
    // update execute in one actor turn. A Skip+Notarize pair is intentionally
    // allowed by Simplex; only Finalize+Skip is prohibited by try_vote_final().
    if (abandon_obsolete()) {
      co_return {};
    }

    slot.state->voted_notar = candidate->id;

    owning_bus().publish<BroadcastVote>(NotarizeVote{candidate->id}).start().detach();
    try_vote_final(slot);  // If we've observed NotarCert already, it might be possible to vote final.
    co_return {};
  }

  td::actor::Task<> process_notarization_observed(BusHandle, std::shared_ptr<const NotarizationObserved> event) {
    auto slot = state_->slot_at(event->id.slot);
    if (!slot.has_value()) {
      co_return {};
    }

    if (timeout_slot_ <= event->id.slot + 1) {
      if ((event->id.slot + 1) % slots_per_leader_window_ == 0) {
        // If we are at the end of the window, we defer setting timeout to LeaderWindowObserved.
        // Note that the condition `timeout_slot_ <= event->id.slot` can't be true if
        // LeaderWindowObserver for the next slot already ran.
        timeout_slot_ = event->id.slot + 1;
      } else {
        // Otherwise, we want to set timeout for notarization of the slot following this one.
        timeout_slot_ = event->id.slot + 2;
      }

      // In the first case above, alarm_timestamp() is very likely to be at this position via
      // NotarCert of the previous slot but in case we missed the certificate let's give the
      // certificate as much time as protocol allows to arrive.
      if (max_tps_mode_enabled()) {
        // Each successful notarization starts a fresh failure budget for the
        // next work-driven candidate. This timestamp is never a production
        // interval and is cancelled/rearmed by protocol progress.
        timeout_base_ = td::Timestamp::now();
        alarm_timestamp() = td::Timestamp::in(max_tps_candidate_timeout(), timeout_base_);
      } else {
        alarm_timestamp() = td::Timestamp::in(
            (timeout_slot_ - current_window_ * slots_per_leader_window_) * params_.target_rate, timeout_base_);
      }
    }

    slot->state->notar_cert = event->id;
    try_vote_final(*slot);
    co_return {};
  }

  void try_vote_final(State::SlotRef slot) {
    CHECK(slot.state->voted_notar || slot.state->notar_cert);

    if (!slot.state->voted_skip && !slot.state->voted_final && slot.state->voted_notar == slot.state->notar_cert) {
      owning_bus().publish<BroadcastVote>(FinalizeVote{*slot.state->voted_notar}).start().detach();
      slot.state->voted_final = true;
    }
  }

  td::uint32 slots_per_leader_window_;
  NewConsensusConfig::NoncriticalParams params_;

  td::Timestamp timeout_base_;
  td::uint32 timeout_slot_ = 0;  // By alarm_timestamp(), slots < timeout_slot_ should be notarized.
  std::chrono::duration<double> first_block_timeout_;
  bool previous_window_had_skip_ = false;
  std::optional<State> state_;
  td::uint32 current_window_ = 0;
  td::uint32 first_active_slot_ = 0;
  td::uint64 generation_epoch_ = 0;
};

}  // namespace

void Consensus::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<ConsensusImpl>("SimplexConsensus");
}

}  // namespace ton::validator::consensus::simplex
