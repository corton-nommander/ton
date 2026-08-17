/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/SharedFuture.h"
#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/utils/CancellationToken.h"

#include "bus.h"
#include "stats.h"
#include "utils.h"

#include <algorithm>
#include <iterator>

namespace ton::validator::consensus {

namespace {

// Leave enough local actor time to report an idle queue before the Collator's
// hard alarm.  The remaining (larger) outer consensus margin is computed by
// max_tps_candidate_work_timeout().  This is completion slack, not pacing.
static constexpr std::chrono::milliseconds MAX_TPS_IDLE_COMPLETION_MARGIN{50};

void merge_external_hashes(std::vector<Bits256>& target, std::vector<Bits256> added) {
  if (added.empty()) {
    return;
  }
  target.insert(target.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
  std::sort(target.begin(), target.end());
  target.erase(std::unique(target.begin(), target.end()), target.end());
}

class BlockProducerImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    target_rate_ = owning_bus()->config.noncritical_params.target_rate;
    no_empty_blocks_on_error_timeout_ = owning_bus()->config.noncritical_params.no_empty_blocks_on_error_timeout;
    max_tps_mode_ = max_tps_mode_enabled();
    if (max_tps_mode_) {
      LOG(WARNING) << "Simplex block producer throughput mode: work-driven, target_rate=" << target_rate_.count()
                   << "ms, candidate_failure_timeout=" << max_tps_candidate_timeout().count() << "ms";
      LOG(WARNING) << "Simplex local candidate work budget: " << max_tps_candidate_work_timeout().count()
                   << "ms; remaining consensus margin="
                   << (max_tps_candidate_timeout() - max_tps_candidate_work_timeout()).count() << "ms";
    } else {
      LOG(WARNING) << "Simplex block producer throughput mode: target-rate-paced, target_rate="
                   << target_rate_.count() << "ms";
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const NoncriticalParamsUpdated> event) {
    target_rate_ = event->params.target_rate;
    no_empty_blocks_on_error_timeout_ = event->params.no_empty_blocks_on_error_timeout;
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const Start> event) {
    td::uint32 seqno = event->state->next_seqno() - 1;
    last_mc_finalized_seqno_ = std::max(last_mc_finalized_seqno_, seqno);
    last_consensus_finalized_seqno_ = std::max(last_consensus_finalized_seqno_, seqno);
    last_consensus_finalized_at_ = td::Timestamp::now();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    current_leader_window_ = std::nullopt;
    current_slot_ = std::nullopt;
    cancellation_source_.cancel();
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const ConsensusSlotAdvanced> event) {
    if (event->first_active_slot <= first_active_slot_) {
      return;
    }
    first_active_slot_ = event->first_active_slot;
    bool generation_is_obsolete =
        (current_slot_ && *current_slot_ < event->first_active_slot) ||
        (current_leader_window_ && *current_leader_window_ < event->first_active_slot);
    if (generation_is_obsolete) {
      ++generation_epoch_;
      LOG(WARNING) << "Cancelling collation for obsolete slot "
                   << (current_slot_ ? static_cast<td::int64>(*current_slot_) : -1)
                   << "; first active consensus slot is " << event->first_active_slot;
      current_leader_window_ = std::nullopt;
      current_slot_ = std::nullopt;
      cancellation_source_.cancel();
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const FinalizeBlock> event) {
    if (event->signatures->is_final()) {
      last_consensus_finalized_seqno_ = std::max(last_consensus_finalized_seqno_, event->candidate->block_id().seqno());
      last_consensus_finalized_at_ = td::Timestamp::now();
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const OurLeaderWindowStarted> event) {
    if (event->start_slot < first_active_slot_ ||
        (last_started_window_ && event->start_slot <= last_started_window_.value())) {
      LOG(WARNING) << "Ignoring stale or duplicate local leader window at slot " << event->start_slot
                   << "; first_active_slot=" << first_active_slot_
                   << " last_started_window="
                   << (last_started_window_ ? static_cast<td::int64>(last_started_window_.value()) : -1);
      return;
    }

    if (current_leader_window_) {
      cancellation_source_.cancel();
    }

    current_leader_window_ = event->start_slot;
    last_started_window_ = event->start_slot;
    ++generation_epoch_;
    cancellation_source_ = td::CancellationTokenSource();
    generate_candidates(event).start().detach();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BlockFinalizedInMasterchain> event) {
    last_mc_finalized_seqno_ = std::max(event->block.seqno(), last_mc_finalized_seqno_);
    last_consensus_finalized_seqno_ = std::max(last_mc_finalized_seqno_, last_consensus_finalized_seqno_);
  }

 private:
  bool should_generate_empty_block(const ChainStateRef& state) {
    if (state->is_before_split()) {
      return true;
    }
    if (owning_bus()->shard.is_masterchain()) {
      return last_consensus_finalized_seqno_ + 1 < state->next_seqno();
    } else {
      return last_mc_finalized_seqno_ + 8 < state->next_seqno();
    }
  }

  td::actor::Task<> generate_candidates(std::shared_ptr<const OurLeaderWindowStarted> event) {
    auto& bus = *owning_bus();

    auto window = current_leader_window_;
    if (window == std::nullopt) {
      co_return {};
    }
    auto generation_epoch = generation_epoch_;

    ChainStateRef state = event->state;
    ParentId parent = event->base;
    auto branch_excluded_ext_messages = event->excluded_ext_messages;
    bool block_generation_active = false;
    td::actor::SharedFuture<GeneratedCandidate> block_generation;

    std::chrono::milliseconds hard_timeout =
        max_tps_mode_ ? max_tps_candidate_work_timeout()
                      : std::max(target_rate_ * 3, std::chrono::milliseconds(60'000));
    std::chrono::milliseconds start_collate_before =
        max_tps_mode_ || bus.shard.is_masterchain() ? std::chrono::milliseconds(0) : target_rate_;
    td::Timestamp slot_start = event->start_time;

    for (td::uint32 slot = event->start_slot;
         current_leader_window_ == window && generation_epoch_ == generation_epoch && slot < event->end_slot;
         ++slot) {
      current_slot_ = slot;
      if (!max_tps_mode_) {
        co_await td::actor::coro_sleep(slot_start - start_collate_before);
      }
      if (current_leader_window_ != window || generation_epoch_ != generation_epoch || slot < first_active_slot_) {
        break;
      }
      bool is_first_block = !parent.has_value();
      if (!block_generation_active && (!should_generate_empty_block(state) || is_first_block)) {
        block_generation_active = true;
        auto collate_started_at = max_tps_mode_ ? td::Timestamp::now() : slot_start;
        CollateParams params{
            .shard = bus.shard,
            .min_masterchain_block_id = state->min_mc_block_id(),
            .prev = state->block_ids(),
            .creator = Ed25519_PublicKey{bus.local_id.key.ed25519_value().raw()},
            .utime = collate_started_at.at_unix(),
            .hard_timeout = collate_started_at + hard_timeout,
            .excluded_ext_messages = branch_excluded_ext_messages,
            .prev_block_data = state->block_data(),
            .prev_block_state_roots = state->state(),
        };
        if (max_tps_mode_) {
          // Keep the callback open so an idle sidechain waits for work instead
          // of spinning empty blocks.  The first block of a consensus session
          // is the exception: it must be materialized promptly to bootstrap
          // the session, so it drains only the initial synchronous snapshot.
          // Once native work is consumed, the collator uses a tiny queue-idle
          // grace and publishes immediately; this is not block-rate pacing.
          params.soft_timeout = collate_started_at + hard_timeout;
          // Basechain native candidates wait for the first queued transfer.
          // Masterchain candidates have no native ingress and must seal after
          // their current shard/config work; making them wait here would hold
          // every basechain anchor for the full safety timeout.
          if (!is_first_block && !bus.shard.is_masterchain()) {
            // Keep one live native-ingress waiter for essentially the entire
            // local candidate work budget.  The first item wakes it
            // immediately; an empty queue completes quietly just before the
            // Collator alarm and lets the outer Simplex failure deadline
            // advance the idle leader window.
            params.wait_externals_until =
                params.hard_timeout - std::min(MAX_TPS_IDLE_COMPLETION_MARGIN, hard_timeout / 4);
            params.ext_msg_callback_until = params.hard_timeout;
          }
        } else if (bus.shard.is_masterchain()) {
          params.soft_timeout = slot_start + target_rate_;
        } else {
          auto shard_external_wait = std::max(target_rate_ / 4, std::chrono::milliseconds(1));
          auto shard_soft_timeout = std::max(target_rate_ / 2, shard_external_wait);
          params.soft_timeout = slot_start + shard_soft_timeout;
          params.wait_externals_until = slot_start + shard_external_wait;
        }
        block_generation = td::actor::ask(bus.manager, &ManagerFacade::collate_block, std::move(params),
                                          cancellation_source_.get_cancellation_token());
        owning_bus().publish<TraceEvent>(stats::CollateStarted::create(slot));
      }
      if (!max_tps_mode_) {
        co_await td::actor::coro_sleep(slot_start);
      }

      std::optional<GeneratedCandidate> generated_candidate;
      if (block_generation_active) {
        auto candidate_deadline = max_tps_mode_ ? td::Timestamp::now() + hard_timeout : slot_start + target_rate_;
        auto r_candidate = co_await td::actor::await_with_timeout(block_generation.get(), candidate_deadline).wrap();
        if (r_candidate.is_error() && max_tps_mode_ && !bus.shard.is_masterchain() &&
            is_native_collation_idle_status(r_candidate.error())) {
          // Expected work-driven idle completion.  Do not retry this slot and
          // do not manufacture an empty candidate: Simplex's independently
          // armed failure alarm will skip/advance the window for liveness.
          LOG(INFO) << "Native ingress idle for leader window starting at slot " << *window
                    << "; waiting for Simplex failure deadline";
          block_generation_active = false;
          break;
        }
        // The first block in the session cannot be empty
        bool allow_empty =
            !is_first_block && !(last_consensus_finalized_at_ + no_empty_blocks_on_error_timeout_).is_in_past();
        if (r_candidate.is_error() && !allow_empty) {
          LOG(WARNING) << "Generating the first block: "
                       << (r_candidate.error().code() == td::actor::AWAIT_TIMEOUT_CODE
                               ? "takes too long"
                               : r_candidate.error().to_string())
                       << ", don't generate empty block "
                       << (is_first_block ? "(first block)" : "(no finalized blocks for too long)");
          --slot;
          if (r_candidate.error().code() != td::actor::AWAIT_TIMEOUT_CODE) {
            block_generation_active = false;
            co_await td::actor::coro_sleep(td::Timestamp::in(0.1));
          }
          slot_start = std::max(slot_start, td::Timestamp::now());
          continue;
        }
        if (r_candidate.is_ok()) {
          generated_candidate = r_candidate.move_as_ok();
          block_generation_active = false;
        } else if (r_candidate.error().code() == td::actor::AWAIT_TIMEOUT_CODE) {
          generated_candidate = std::nullopt;
          LOG(WARNING) << "Generating an empty block for slot " << slot << ": block collation takes too long";
        } else {
          generated_candidate = std::nullopt;
          LOG(WARNING) << "Generating an empty block for slot " << slot << ": collation error: " << r_candidate.error();
          block_generation_active = false;
        }
      } else {
        generated_candidate = std::nullopt;
        LOG(WARNING) << "Generating an empty block for slot " << slot << ": new_seqno=" << state->next_seqno()
                     << ", last_consensus_finalized_seqno_=" << last_consensus_finalized_seqno_
                     << ", last_mc_finalized_seqno_=" << last_mc_finalized_seqno_
                     << ", before_split=" << state->is_before_split();
      }
      if (current_leader_window_ != window || generation_epoch_ != generation_epoch || slot < first_active_slot_) {
        break;
      }

      CandidateId id;
      std::variant<BlockIdExt, BlockCandidate> block;
      std::optional<adnl::AdnlNodeIdShort> collator;
      if (generated_candidate.has_value()) {
        auto native_hashes = get_candidate_native_external_hashes(generated_candidate->candidate);
        if (native_hashes.is_error()) {
          co_return native_hashes.move_as_error_prefix("cannot track speculative native messages: ");
        }
        merge_external_hashes(branch_excluded_ext_messages, native_hashes.move_as_ok());
        td::actor::send_closure(bus.manager, &ManagerFacade::cache_block_candidate,
                                generated_candidate->candidate.clone());
        state = state->apply(generated_candidate->candidate);
        block = std::move(generated_candidate->candidate);
        if (!generated_candidate->collator_node_id.is_zero()) {
          collator = adnl::AdnlNodeIdShort{generated_candidate->collator_node_id};
        }
        id = CandidateHashData::create_full(generated_candidate->candidate, parent).build_id_with(slot);
        owning_bus().publish<TraceEvent>(stats::CollateFinished::create(slot, id));
      } else {
        CHECK(parent.has_value());
        auto referenced_block = state->assert_normal();
        block = referenced_block;
        id = CandidateHashData::create_empty(referenced_block, *parent).build_id_with(slot);
        owning_bus().publish<TraceEvent>(stats::CollatedEmpty::create(id));
      }

      auto id_to_sign = serialize_tl_object(id.to_tl(), true);
      auto data_to_sign = create_serialize_tl_object<tl::dataToSign>(bus.session_id, std::move(id_to_sign));
      auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                               std::move(data_to_sign));
      auto candidate = td::make_ref<Candidate>(id, parent, bus.local_id.idx, std::move(block), std::move(signature));
      if (current_leader_window_ != window || generation_epoch_ != generation_epoch || slot < first_active_slot_) {
        break;
      }
      owning_bus().publish<CandidateGenerated>(candidate, collator);
      owning_bus().publish<CandidateReceived>(candidate);
      owning_bus().publish<TraceEvent>(stats::CandidateReceived::create(candidate, true));
      parent = id;

      slot_start = max_tps_mode_ ? td::Timestamp::now() : slot_start + target_rate_;
    }

    if (current_leader_window_ == window && generation_epoch_ == generation_epoch) {
      current_leader_window_ = std::nullopt;
      current_slot_ = std::nullopt;
    }

    co_return {};
  }

  std::optional<td::uint32> current_leader_window_;
  std::optional<td::uint32> last_started_window_;
  std::optional<td::uint32> current_slot_;
  td::uint32 first_active_slot_{0};
  td::uint64 generation_epoch_{0};
  td::CancellationTokenSource cancellation_source_;

  BlockSeqno last_consensus_finalized_seqno_ = 0;
  BlockSeqno last_mc_finalized_seqno_ = 0;
  std::chrono::milliseconds target_rate_;
  bool max_tps_mode_{false};

  std::chrono::milliseconds no_empty_blocks_on_error_timeout_;
  td::Timestamp last_consensus_finalized_at_;
};

}  // namespace

void BlockProducer::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<BlockProducerImpl>("BlockProducer");
}

}  // namespace ton::validator::consensus
