/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/utils.h"
#include "td/actor/SharedFuture.h"
#include "td/actor/coro_utils.h"

#include "bus.h"
#include "state-resolver-policy.h"

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
  void handle(BusHandle, std::shared_ptr<const BlockFinalizedInMasterchain> event) {
    on_block_finalized_in_masterchain(event->block);
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
    td::uint64 exclusion_epoch = 0;
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
      td::Result<ResolvedState> result;
      do {
        entry.exclusion_epoch = native_exclusion_epoch_;
        result = co_await resolve_state_inner(id).wrap();
        if (result.is_ok()) {
          merge_external_hashes(result.ok_ref().excluded_ext_messages, unanchored_finalized_native_hashes_);
        }
        // A masterchain notification can release hashes while this recursive
        // resolution is suspended. Re-resolve instead of caching (or returning)
        // a mixture of the old and new canonical branches.
      } while (result.is_ok() && entry.exclusion_epoch != native_exclusion_epoch_);
      for (auto& p : entry.promises) {
        p.set_result(result.clone());
      }
      entry.promises.clear();
      if (result.is_ok()) {
        entry.result = result.move_as_ok();
      } else {
        state_cache_.erase(id);
      }
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
      // The external-message pool still contains native messages until the
      // exact block is anchored in masterchain. A locally finalized state must
      // therefore carry exclusions just like a speculative parent chain.
      merge_external_hashes(native_hashes, unanchored_finalized_native_hashes_);
      co_return ResolvedState{state, gen_utime_exact, std::move(native_hashes)};
    }

    auto prev_data_state = co_await resolve_state(candidate->parent_id);
    merge_external_hashes(prev_data_state.excluded_ext_messages, std::move(native_hashes));
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

  struct UnanchoredFinalizedBlock {
    BlockIdExt block_id;
    std::vector<Bits256> native_hashes;
  };

  // FinalizeBlock means that this validator accepted the candidate locally;
  // it does not yet prove which candidate at this seqno was anchored by the
  // masterchain. Keep all such messages excluded until the exact canonical top
  // arrives. The per-block ledger lets that notification release old/orphaned
  // hashes without releasing later, still-unanchored candidates.
  std::vector<UnanchoredFinalizedBlock> unanchored_finalized_blocks_;
  std::vector<Bits256> unanchored_finalized_native_hashes_;
  std::optional<BlockIdExt> last_masterchain_finalized_block_;
  td::uint64 native_exclusion_epoch_ = 0;

  void release_unanchored_native_hashes_through(const BlockIdExt& block_id) {
    auto first_retained = std::stable_partition(
        unanchored_finalized_blocks_.begin(), unanchored_finalized_blocks_.end(),
        [&](const auto& entry) { return entry.block_id.seqno() > block_id.seqno(); });
    if (first_retained == unanchored_finalized_blocks_.end()) {
      return;
    }

    auto released_blocks = static_cast<std::size_t>(unanchored_finalized_blocks_.end() - first_retained);
    unanchored_finalized_blocks_.erase(first_retained, unanchored_finalized_blocks_.end());
    unanchored_finalized_native_hashes_.clear();
    for (const auto& entry : unanchored_finalized_blocks_) {
      merge_external_hashes(unanchored_finalized_native_hashes_, entry.native_hashes);
    }

    ++native_exclusion_epoch_;
    for (auto it = state_cache_.begin(); it != state_cache_.end();) {
      if (it->second.result) {
        it = state_cache_.erase(it);
      } else {
        ++it;
      }
    }
    LOG(INFO) << "Released locally-finalized native exclusions at canonical top " << block_id.to_str()
              << ": blocks=" << released_blocks
              << " retained_blocks=" << unanchored_finalized_blocks_.size()
              << " retained_hashes=" << unanchored_finalized_native_hashes_.size();
  }

  void record_unanchored_finalized_native_hashes(BlockIdExt block_id, std::vector<Bits256> hashes) {
    if (last_masterchain_finalized_block_) {
      auto relation = classify_native_finalization(block_id.seqno(), last_masterchain_finalized_block_->seqno(),
                                                   block_id == *last_masterchain_finalized_block_);
      if (relation != NativeFinalizationRelation::after_canonical_top) {
        // The shard-top notification can race ahead while accept_block is
        // suspended, and it may advance past several locally finalized blocks.
        // Every height at or below that top is already decided: the exact root
        // is canonical and every other root is losing work. None may become a
        // new exclusion after the canonical release.
        return;
      }
    }
    if (hashes.empty()) {
      return;
    }
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());

    auto entry = std::find_if(unanchored_finalized_blocks_.begin(), unanchored_finalized_blocks_.end(),
                              [&](const auto& item) { return item.block_id == block_id; });
    if (entry == unanchored_finalized_blocks_.end()) {
      unanchored_finalized_blocks_.push_back({block_id, hashes});
    } else {
      merge_external_hashes(entry->native_hashes, hashes);
    }
    for (auto& [_, cached] : state_cache_) {
      if (cached.result) {
        merge_external_hashes(cached.result->excluded_ext_messages, hashes);
      }
    }
    merge_external_hashes(unanchored_finalized_native_hashes_, std::move(hashes));
  }

  void on_block_finalized_in_masterchain(BlockIdExt block_id) {
    if (block_id.shard_full() != owning_bus()->shard || block_id.seqno() == 0) {
      return;
    }

    if (last_masterchain_finalized_block_) {
      if (block_id.seqno() < last_masterchain_finalized_block_->seqno()) {
        return;
      }
      if (block_id.seqno() == last_masterchain_finalized_block_->seqno()) {
        if (block_id != *last_masterchain_finalized_block_) {
          LOG(ERROR) << "Ignoring conflicting masterchain-finalized block at seqno " << block_id.seqno()
                     << ": current=" << last_masterchain_finalized_block_->to_str()
                     << " received=" << block_id.to_str();
        }
        return;
      }
    }
    last_masterchain_finalized_block_ = block_id;

    // The canonical top decides the whole prefix, including losing roots at
    // its own height and ancestors finalized late by an overlapping session.
    // Canonical messages are independently purged by ExtMessagePool account
    // reconciliation; losing messages must be allowed back into descendants.
    release_unanchored_native_hashes_through(block_id);
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

      // Decode compact native metadata once. FinalizeBlock value-owns the
      // tracked identities for BlockAccepter, while this separate hash vector
      // survives the publish await for the masterchain-race exclusion guard.
      auto finalized_native_messages =
          get_candidate_native_external_messages(std::get<BlockCandidate>(candidate->block)).move_as_ok();
      std::vector<Bits256> finalized_native_hashes;
      finalized_native_hashes.reserve(finalized_native_messages.size());
      for (const auto& message : finalized_native_messages) {
        finalized_native_hashes.push_back(message.hash);
      }
      // The decoder already returns this order, but retain the resolver's
      // legacy sort/dedup guarantee independently of that implementation.
      std::sort(finalized_native_hashes.begin(), finalized_native_hashes.end());
      finalized_native_hashes.erase(
          std::unique(finalized_native_hashes.begin(), finalized_native_hashes.end()), finalized_native_hashes.end());

      td::Ref<block::BlockSignatureSet> sig_set;
      if (final_cert) {
        sig_set = (*final_cert)->to_signature_set(*final_candidate, bus);
      } else {
        sig_set = notar_cert->to_signature_set(candidate, bus);
      }
      co_await owning_bus().publish<FinalizeBlock>(candidate, sig_set, std::move(finalized_native_messages));
      record_unanchored_finalized_native_hashes(candidate->block_id(), std::move(finalized_native_hashes));
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
