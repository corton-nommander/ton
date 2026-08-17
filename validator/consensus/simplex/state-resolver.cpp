/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/utils.h"
#include "td/actor/SharedFuture.h"
#include "td/actor/coro_utils.h"

#include "bus.h"

#include <algorithm>
#include <iterator>

namespace ton::validator::consensus::simplex {

namespace tl {

using db_key_finalizedBlock = ton_api::consensus_simplex_db_key_finalizedBlock;
using db_key_finalizedBlockRef = tl_object_ptr<db_key_finalizedBlock>;

}  // namespace tl

namespace {

void merge_external_hashes(std::vector<Bits256>& target, std::vector<Bits256> added) {
  if (added.empty()) {
    return;
  }
  target.insert(target.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
  std::sort(target.begin(), target.end());
  target.erase(std::unique(target.begin(), target.end()), target.end());
}

void remove_external_hashes(std::vector<Bits256>& target, const std::vector<Bits256>& removed) {
  if (target.empty() || removed.empty()) {
    return;
  }
  std::vector<Bits256> retained;
  retained.reserve(target.size());
  std::set_difference(target.begin(), target.end(), removed.begin(), removed.end(), std::back_inserter(retained));
  target = std::move(retained);
}

class StateResolverImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
  using ResolvedState = ResolveState::Result;

 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    auto [awaiter, promise] = td::actor::StartedTask<StartEvent>::make_bridge();
    genesis_promise_ = std::move(promise);
    genesis_ = std::move(awaiter);

    auto data = owning_bus()->db->get_by_prefix(ton_api::consensus_simplex_db_key_finalizedBlock::ID);
    for (auto& [key_str, _] : data) {
      auto key = fetch_tl_object<ton_api::consensus_simplex_db_key_finalizedBlock>(key_str, true).ensure().move_as_ok();
      finalized_blocks_[CandidateId::from_tl(key->candidateId_)].done = true;
    }
    LOG(INFO) << "Loaded " << data.size() << " finalized blocks from DB";
  }

  void tear_down() override {
    genesis_promise_.set_error(td::Status::Error(ErrorCode::cancelled, "cancelled"));
    for (auto& [_, s] : state_cache_) {
      for (auto& p : s.promises) {
        p.set_error(td::Status::Error(ErrorCode::cancelled, "cancelled"));
      }
    }
    for (auto& [_, s] : finalized_blocks_) {
      for (auto& p : s.waiters) {
        p.set_error(td::Status::Error(ErrorCode::cancelled, "cancelled"));
      }
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const Start> event) {
    genesis_promise_.set_value(std::move(event));
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const FinalizationObserved> event) {
    finalize_blocks(event->id, event->certificate, std::nullopt).start().detach();
  }

  template <>
  td::actor::Task<ResolvedState> process(BusHandle, std::shared_ptr<ResolveState> request) {
    co_return co_await resolve_state(request->id);
  }

 private:
  // ===== State resolution =====
  struct CachedState {
    std::optional<ResolvedState> result;
    bool started = false;
    std::vector<td::Promise<ResolvedState>> promises;
  };

  td::Promise<StartEvent> genesis_promise_;
  td::actor::SharedFuture<StartEvent> genesis_;

  std::map<ParentId, CachedState> state_cache_;

  td::actor::Task<ResolvedState> resolve_state(ParentId id) {
    CachedState& entry = state_cache_[id];
    if (entry.result.has_value()) {
      co_return *entry.result;
    }
    auto [task, promise] = td::actor::StartedTask<ResolvedState>::make_bridge();
    entry.promises.push_back(std::move(promise));
    if (!entry.started) {
      entry.started = true;
      auto result = co_await resolve_state_inner(id).wrap();
      if (result.is_ok()) {
        remove_external_hashes(result.ok_ref().excluded_ext_messages, finalized_native_hashes_);
      }
      for (auto& p : entry.promises) {
        p.set_result(result.clone());
      }
      entry.promises.clear();
      if (result.is_ok()) {
        entry.result = result.move_as_ok();
      } else {
        state_cache_.erase(id);
      }
      maybe_clear_finalized_native_hashes();
    }
    co_return co_await std::move(task);
  }

  bool is_finalized(CandidateId id) {
    auto it = finalized_blocks_.find(id);
    return it != finalized_blocks_.end() && it->second.done;
  }

  td::actor::Task<ResolvedState> resolve_state_inner(ParentId id) {
    if (!id.has_value()) {
      auto genesis = co_await genesis_.get();
      auto state = co_await ChainState::from_manager(owning_bus()->manager, owning_bus()->shard,
                                                     genesis->state->block_ids(), genesis->state->min_mc_block_id());
      co_return ResolvedState{state, std::nullopt, {}};
    }

    auto candidate = (co_await owning_bus().publish<ResolveCandidate>(*id)).candidate;
    if (candidate->is_empty()) {
      co_return co_await resolve_state(candidate->parent_id);
    }
    auto gen_utime_exact = get_candidate_gen_utime_exact(std::get<BlockCandidate>(candidate->block)).move_as_ok();
    auto native_hashes =
        get_candidate_native_external_hashes(std::get<BlockCandidate>(candidate->block)).move_as_ok();

    if (is_finalized(*id)) {
      auto genesis = co_await genesis_.get();
      auto state = co_await ChainState::from_manager(owning_bus()->manager, owning_bus()->shard,
                                                     {candidate->block_id()}, genesis->state->min_mc_block_id());
      co_return ResolvedState{state, gen_utime_exact, {}};
    }

    auto prev_data_state = co_await resolve_state(candidate->parent_id);
    merge_external_hashes(prev_data_state.excluded_ext_messages, std::move(native_hashes));
    remove_external_hashes(prev_data_state.excluded_ext_messages, finalized_native_hashes_);
    co_return ResolvedState{
        .state = prev_data_state.state->apply(std::get<BlockCandidate>(candidate->block)),
        .gen_utime_exact = gen_utime_exact,
        .excluded_ext_messages = std::move(prev_data_state.excluded_ext_messages),
    };
  }

  // ===== Block finalization =====
  struct FinalizedBlock {
    bool done = false;
    bool started = false;
    std::vector<td::Promise<td::Unit>> waiters;
  };

  std::map<CandidateId, FinalizedBlock> finalized_blocks_;

  // Normally finalized hashes are scrubbed from every cached result
  // immediately. Keep this temporary set only while state resolutions that
  // started before finalization are still in flight.
  std::vector<Bits256> finalized_native_hashes_;

  void record_finalized_native_hashes(std::vector<Bits256> hashes) {
    if (hashes.empty()) {
      return;
    }
    merge_external_hashes(finalized_native_hashes_, std::move(hashes));
    for (auto& [_, cached] : state_cache_) {
      if (cached.result) {
        remove_external_hashes(cached.result->excluded_ext_messages, finalized_native_hashes_);
      }
    }
    maybe_clear_finalized_native_hashes();
  }

  void maybe_clear_finalized_native_hashes() {
    // finalize_blocks_inner records hashes after accept_block, but the outer
    // coroutine marks the candidate done only after its finalized DB marker is
    // durable.  Keep the scrub set across that await so a concurrently started
    // ResolveState cannot recache the just-finalized ancestor exclusions.
    for (const auto& [_, finalized] : finalized_blocks_) {
      if (finalized.started && !finalized.done) {
        return;
      }
    }
    for (const auto& [_, cached] : state_cache_) {
      if (cached.started && !cached.result) {
        return;
      }
    }
    finalized_native_hashes_.clear();
  }

  td::actor::Task<> finalize_blocks(CandidateId id, std::optional<FinalCertRef> final_cert,
                                    std::optional<CandidateRef> final_candidate) {
    FinalizedBlock& state = finalized_blocks_[id];
    if (state.done) {
      co_return {};
    }
    auto [task, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
    state.waiters.push_back(std::move(promise));
    if (!state.started) {
      state.started = true;
      auto result = co_await finalize_blocks_inner(id, final_cert, final_candidate).wrap();
      for (auto& p : state.waiters) {
        p.set_result(result.clone());
      }
      state.waiters.clear();
      if (result.is_ok()) {
        state.done = true;
      } else {
        finalized_blocks_.erase(id);
      }
      maybe_clear_finalized_native_hashes();
    }
    co_return co_await std::move(task);
  }

  td::actor::Task<> finalize_blocks_inner(CandidateId id, std::optional<FinalCertRef> final_cert,
                                          std::optional<CandidateRef> final_candidate) {
    auto& bus = *owning_bus();

    if (!final_cert && bus.shard.is_masterchain()) {
      co_return {};
    }

    auto [candidate, notar_cert] = co_await owning_bus().publish<ResolveCandidate>(id);
    if (final_cert && !final_candidate) {
      CHECK((*final_cert)->vote.id == id);
      final_candidate = candidate;
    }

    if (!candidate->is_empty()) {
      if (auto parent = candidate->parent_id) {
        co_await finalize_blocks(*parent, std::nullopt, std::nullopt);
      }

      auto finalized_native_hashes =
          get_candidate_native_external_hashes(std::get<BlockCandidate>(candidate->block)).move_as_ok();

      td::Ref<block::BlockSignatureSet> sig_set;
      if (final_cert) {
        sig_set = (*final_cert)->to_signature_set(*final_candidate, bus);
      } else {
        sig_set = notar_cert->to_signature_set(candidate, bus);
      }
      co_await owning_bus().publish<FinalizeBlock>(candidate, sig_set);
      record_finalized_native_hashes(std::move(finalized_native_hashes));
    } else {
      if (auto parent = candidate->parent_id) {
        co_await finalize_blocks(*parent, final_cert, final_candidate);
      }
    }

    auto key = create_serialize_tl_object<tl::db_key_finalizedBlock>(id.to_tl());
    co_await bus.db->set(std::move(key), td::BufferSlice());
    co_return {};
  }
};

}  // namespace

void StateResolver::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<StateResolverImpl>("StateResolver");
}

}  // namespace ton::validator::consensus::simplex
