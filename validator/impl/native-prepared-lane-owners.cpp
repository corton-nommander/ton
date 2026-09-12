/*
    This file is part of TON Blockchain Library.
    Licensed under the GNU Lesser General Public License, version 2 or later.
*/
#include "ext-message-pool.hpp"
#include "fabric.h"
#include "validator/native-lane-scheduler.h"
#include "ton/ton-io.hpp"
#include "vm/cellslice.h"

namespace ton::validator {

td::actor::ActorId<ExtMessagePool::NativeSignatureVerifier> ExtMessagePool::next_native_signature_verifier() {
  if (is_native_lane_owner()) {
    CHECK(!native_lane_owner_.workers.empty());
    return native_lane_owner_.workers[native_signature_verifier_cursor_++ % native_lane_owner_.workers.size()];
  }
  CHECK(!native_signature_verifiers_.empty());
  return native_signature_verifiers_[native_signature_verifier_cursor_++ % native_signature_verifiers_.size()].get();
}

std::size_t ExtMessagePool::checked_message_count(WorkchainId wc, StdSmcAddress addr, bool increment) {
  if (native_lane_owner_.coordination) {
    return increment ? native_lane_owner_.coordination->inc_msg_count(wc, addr)
                     : native_lane_owner_.coordination->get_msg_count(wc, addr);
  }
  return increment ? checked_ext_msg_counter_.inc_msg_count(wc, addr)
                   : checked_ext_msg_counter_.get_msg_count(wc, addr);
}

td::Status ExtMessagePool::check_native_lane_fence(td::uint64 generation) {
  if (!native_lane_owner_.coordination) { return td::Status::OK(); }
  if (!native_lane_owner_.coordination->topology_ready.load(std::memory_order_acquire)) {
    ++native_lane_fence_rejections_;
    return td::Status::Error(ErrorCode::notready, "native pool owners require unchanged fixed payment-lane topology");
  }
  if (native_lane_applied_generation_ != native_lane_owner_.coordination->published_generation.load(std::memory_order_acquire) ||
      (generation != 0 && generation != native_lane_applied_generation_)) {
    ++native_lane_fence_rejections_;
    // Retain the established typed availability diagnostic understood by clients.
    return td::Status::Error(ErrorCode::notready, "native pool owner has not applied the current admission state; retry");
  }
  return td::Status::OK();
}

td::Status ExtMessagePool::check_native_lane_message(const ExtMessage &message, td::uint64 generation) {
  if (!native_lane_owner_.coordination) { return td::Status::OK(); }
  const auto tag = vm::load_cell_slice(message.root_cell()).prefetch_ulong(32);
  const bool native = tag == block::NativeTransfer::magic || tag == block::NativeTransferRun::magic;
  if (!native) {
    return is_native_lane_owner() ? td::Status::Error("generic message was routed to a native admission owner")
                                  : td::Status::OK();
  }
  if (!is_native_lane_owner() || message.wc() != basechainId ||
      native_lane_owner_.coordination->owner_for(message.wc(), message.addr()) != unsigned(native_lane_owner_.index)) {
    return td::Status::Error("native message was routed to the wrong admission owner");
  }
  return check_native_lane_fence(generation);
}

bool ExtMessagePool::native_lane_topology_supported(const NativeAdmissionSnapshot &snapshot) const {
  if (native_lane_topology_failed_ || !snapshot.runs_enabled || !snapshot.payment_lane_policy ||
      snapshot.payment_lane_policy.value().depth() != 2) { return false; }
  std::vector<ShardIdFull> leaves;
  for (const auto &shard : snapshot.state->get_shards()) { leaves.push_back(shard->top_block_id().shard_full()); }
  return NativeLaneSchedulerPlan::supports(leaves);
}

void ExtMessagePool::update_last_masterchain_state(td::Ref<MasterchainState> state) {
  if (!is_native_lane_router()) {
    // Children receive only the checked, supported publication API below.
    CHECK(!is_native_lane_owner());
    update_last_masterchain_state_local(std::move(state));
    return;
  }
  if (state.is_null() || !state->get_block_id().is_masterchain()) {
    update_last_masterchain_state_local(std::move(state));
    return;
  }
  auto &coordination = *native_lane_owner_.coordination;
  if (NativePoolOwnerCoordination::ReadFence::held_on_this_thread(&coordination)) {
    td::actor::send_closure_later(actor_id(this), &ExtMessagePool::update_last_masterchain_state, std::move(state));
    return;
  }
  if (last_masterchain_state_.not_null()) {
    const auto old = last_masterchain_state_->get_block_id(), next = state->get_block_id();
    if (next.seqno() < old.seqno() || (next.seqno() == old.seqno() && next != old)) {
      ++native_batch_ignored_mc_state_updates_;
      return;
    }
  }
  auto exact_root = [](const td::Ref<MasterchainState> &value) {
    return value->root_cell().not_null() ? RootHash{value->root_cell()->get_hash().bits()} : RootHash::zero();
  };
  const bool changed = last_masterchain_state_.is_null() ||
      last_masterchain_state_->get_block_id() != state->get_block_id() || exact_root(last_masterchain_state_) != exact_root(state);
  td::uint64 generation;
  {
    std::unique_lock fence(coordination.state_fence);
    generation = coordination.published_generation.load(std::memory_order_relaxed) + unsigned(changed);
    coordination.published_generation.store(generation, std::memory_order_release);
  }
  // No parser, actor sends, promise settlement, or account work under the writer fence.
  update_last_masterchain_state_local(state);
  CHECK(native_accounts_.empty() && native_nonce_watermarks_.empty() && locally_accepted_native_nonces_.empty());
  auto snapshot = pin_native_admission_snapshot();
  bool supported = snapshot.is_ok() && native_lane_topology_supported(snapshot.ok());
  if (!supported && native_lane_topology_activated_) { native_lane_topology_failed_ = true; }
  supported = supported && !native_lane_topology_failed_;
  if (supported && native_lane_owners_.empty()) {
    // No owner exists during incomplete bootstrap, so no account can land in a
    // temporary coordinator ledger or the wrong scheduler group.
    std::vector<td::actor::ActorId<NativeSignatureVerifier>> workers;
    for (auto &worker : native_signature_verifiers_) { workers.push_back(worker.get()); }
    for (unsigned index = 0; index < 4; ++index) {
      ShardIdFull lane{basechainId, (td::uint64(index) << 62) | (td::uint64{1} << 61)};
      native_lane_owners_.push_back(td::actor::create_actor<ExtMessagePool>(
          native_lane_actor_options("native-admission-owner", lane, NativeLaneActorKind::admission), opts_, manager_,
          NativeLaneOwnerInit{native_lane_owner_.coordination, static_cast<int>(index), workers}));
    }
    native_lane_topology_activated_ = true;
  }
  {
    std::unique_lock fence(coordination.state_fence);
    coordination.topology_ready.store(supported, std::memory_order_release);
    native_lane_applied_generation_ = generation;
  }
  for (auto &owner : native_lane_owners_) {
    if (supported) {
      td::actor::send_closure(owner, &ExtMessagePool::apply_lane_masterchain_state, snapshot.ok(), generation);
    } else {
      // Keep the last supported state, watermarks and reservations in place.
      // Never let the ordinary mode-change purge turn this into migration.
      td::actor::send_closure(owner, &ExtMessagePool::close_native_lane_callbacks);
    }
  }
}

void ExtMessagePool::apply_lane_masterchain_state(NativeAdmissionSnapshot snapshot, td::uint64 generation) {
  CHECK(is_native_lane_owner());
  if (generation < native_lane_applied_generation_ ||
      !native_lane_owner_.coordination->topology_ready.load(std::memory_order_acquire)) { return; }
  CHECK(native_lane_topology_supported(snapshot));
  CHECK(snapshot.state->get_block_id() == snapshot.block_id &&
        RootHash{snapshot.state->root_cell()->get_hash().bits()} == snapshot.state_root);
  // Immutable config facts are shared, account/trie/reconciliation caches are local.
  native_config_cache_.store(snapshot.block_id, snapshot.state_root, snapshot);
  update_last_masterchain_state_local(snapshot.state);
  update_native_transfer_runs_mode(snapshot.runs_enabled);
  update_native_payment_lane_depth(snapshot.payment_lane_policy.value().depth());
  native_lane_applied_generation_ = generation;
}

void ExtMessagePool::close_native_lane_callbacks() {
  for (const auto &callback : callbacks_) { cancel_callback_delivery(callback); }
  callbacks_.clear();
  reset_native_producers();
}

void ExtMessagePool::update_options(td::Ref<ValidatorManagerOptions> opts) {
  opts_ = std::move(opts);
  if (is_native_lane_router()) {
    native_lane_owner_.coordination->set_limit(opts_->max_mempool_num());
    for (auto &owner : native_lane_owners_) { td::actor::send_closure(owner, &ExtMessagePool::update_options, opts_); }
  }
}

td::actor::Task<ExtMessagePool::PreparedBatch> ExtMessagePool::prepare_for_lane_routing(
    std::vector<td::BufferSlice> batch, td::Timestamp deadline) {
  CHECK(is_native_lane_router());
  if ((deadline && deadline.is_in_past()) || last_masterchain_state_.is_null()) {
    PreparedBatch rejected;
    for (std::size_t index = 0; index < batch.size(); ++index) {
      rejected.emplace_back(deadline && deadline.is_in_past()
          ? td::Status::Error(ErrorCode::timeout, "external message admission deadline expired")
          : td::Status::Error(ErrorCode::notready, "not ready"));
    }
    co_return rejected;
  }
  auto prepared = co_await prepare_external_batch(batch, deadline);
  if (prepared) { co_return std::move(prepared.value()); }
  // Budget fallback must still reach the unique owner. Perform strict owned
  // preparation inline without expanding queued-parent/byte or worker budgets.
  // Account authority is still chosen after this work. This is deliberately a
  // bounded fallback, measured separately from queued worker preparation.
  PreparedBatch result;
  result.reserve(batch.size());
  auto context = pin_native_admission_snapshot();
  for (auto &body : batch) {
    if (last_masterchain_state_.is_null()) {
      result.emplace_back(td::Status::Error(ErrorCode::notready, "not ready"));
      continue;
    }
    if (context.is_error()) {
      // Generic RPCs remain usable during native capability/bootstrap errors.
      const auto wire_size = body.size();
      auto message = create_ext_message(std::move(body), last_masterchain_state_->get_ext_msg_limits());
      if (message.is_error()) { result.emplace_back(message.move_as_error()); continue; }
      const auto tag = vm::load_cell_slice(message.ok()->root_cell()).prefetch_ulong(32);
      if (tag == block::NativeTransfer::magic || tag == block::NativeTransferRun::magic) {
        result.emplace_back(context.error().clone());
        continue;
      }
      PreparedExternalMessage generic;
      generic.wire_size_ = wire_size;
      generic.message_ = message.move_as_ok();
      result.emplace_back(std::move(generic));
      continue;
    }
    ++native_lane_inline_preparations_;
    result.push_back(std::move(NativeSignatureVerifier::prepare_owned(std::move(body),
        context.ok().state->get_ext_msg_limits(), context.ok().chain_domain, deadline,
        native_prepare_profile_enabled_).result));
  }
  co_return result;
}

td::actor::Task<ExtMessagePool::BatchCheckResult> ExtMessagePool::route_prepared_lane_batch(
    std::vector<td::BufferSlice> batch, int priority, bool add_to_mempool, td::Timestamp deadline) {
  const auto count = batch.size();
  auto prepared = co_await prepare_for_lane_routing(std::move(batch), deadline);
  BatchCheckResult result;
  result.statuses.resize(count);
  auto snapshot = pin_native_admission_snapshot(); // First spending context is selected AFTER preparation.
  const auto generation = native_lane_applied_generation_;
  auto fence = check_native_lane_fence(generation);
  std::array<PreparedBatch, 5> groups;
  std::array<std::vector<std::size_t>, 5> indices;
  for (std::size_t index = 0; index < prepared.size(); ++index) {
    auto &item = prepared[index];
    if (deadline && deadline.is_in_past()) {
      result.statuses[index] = ExternalMessageAdmissionResult::failure(
          td::Status::Error(ErrorCode::timeout, "external message admission deadline expired"));
      continue;
    }
    if (item.is_error()) { result.statuses[index] = ExternalMessageAdmissionResult::failure(item.move_as_error()); continue; }
    const bool native = item.ok().transfer_ || item.ok().run_;
    if (native && (snapshot.is_error() || fence.is_error())) {
      result.statuses[index] = ExternalMessageAdmissionResult::failure(
          snapshot.is_error() ? snapshot.error().clone() : fence.clone());
      continue;
    }
    if (native && item.ok().message_->wc() != basechainId) {
      result.statuses[index] = ExternalMessageAdmissionResult::failure(td::Status::Error("native transfer is routed to the wrong source account"));
      continue;
    }
    const auto owner = native ? native_lane_owner_.coordination->owner_for(item.ok().message_->wc(), item.ok().message_->addr()) : 4;
    groups[owner].push_back(std::move(item));
    indices[owner].push_back(index);
  }
  std::vector<td::actor::StartedTask<BatchCheckResult>> tasks;
  std::vector<unsigned> active;
  const double dispatched = td::Time::now();
  for (unsigned owner = 0; owner < 5; ++owner) {
    if (groups[owner].empty()) { continue; }
    active.push_back(owner);
    if (owner == 4) {
      std::vector<td::BufferSlice> empty(groups[owner].size());
      tasks.push_back(check_add_external_messages_impl(std::move(empty), priority, add_to_mempool, deadline,
          td::optional<PreparedBatch>{std::move(groups[owner])}).start());
    } else {
      ++native_lane_routed_batches_;
      native_lane_routed_messages_ += groups[owner].size();
      tasks.push_back(td::actor::ask(native_lane_owners_[owner], &ExtMessagePool::check_add_prepared_lane_batch,
          std::move(groups[owner]), priority, add_to_mempool, deadline, snapshot.ok(), generation, dispatched));
    }
  }
  // No outer timeout: an earlier sibling may have committed within its absolute
  // deadline. Always collect its per-input status and broadcast waiter intact.
  auto replies = co_await td::actor::all_wrap(std::move(tasks));
  native_lane_router_join_.observe(td::Time::now() - dispatched);
  for (std::size_t group = 0; group < replies.size(); ++group) {
    auto &reply = replies[group];
    auto &positions = indices[active[group]];
    if (reply.is_error()) {
      for (auto index : positions) { result.statuses[index] = ExternalMessageAdmissionResult::failure(reply.error().clone()); }
      continue;
    }
    auto value = reply.move_as_ok();
    CHECK(value.statuses.size() == positions.size());
    for (std::size_t j = 0; j < positions.size(); ++j) { result.statuses[positions[j]] = std::move(value.statuses[j]); }
    for (auto &checked : value.checked_messages) { result.checked_messages.push_back(std::move(checked)); }
  }
  CHECK(native_accounts_.empty() && native_nonce_watermarks_.empty());
  co_return result;
}

td::actor::Task<ExtMessagePool::CheckResult> ExtMessagePool::route_prepared_lane_single(
    td::BufferSlice body, int priority, bool add_to_mempool, td::Timestamp deadline) {
  std::vector<td::BufferSlice> inputs;
  inputs.push_back(std::move(body));
  auto prepared = co_await prepare_for_lane_routing(std::move(inputs), deadline);
  if (prepared.front().is_error()) { co_return prepared.front().move_as_error(); }
  auto item = prepared.front().move_as_ok();
  if (!item.transfer_ && !item.run_) {
    const auto limits = last_masterchain_state_->get_ext_msg_limits();
    if (item.wire_size_ > limits.max_size) { co_return td::Status::Error("external message too large, rejecting"); }
    if (item.message_->root_cell()->get_depth() >= limits.max_depth) { co_return td::Status::Error("external message is too deep"); }
    co_return co_await check_add_parsed_external_message_until(item.message_, priority, add_to_mempool, deadline, &item);
  }
  auto snapshot = pin_native_admission_snapshot();
  if (snapshot.is_error()) { co_return snapshot.move_as_error(); }
  auto fence = check_native_lane_fence(native_lane_applied_generation_);
  if (fence.is_error()) { co_return std::move(fence); }
  if (item.message_->wc() != basechainId) { co_return td::Status::Error("native transfer is routed to the wrong source account"); }
  auto owner = native_lane_owner_.coordination->owner_for(item.message_->wc(), item.message_->addr());
  ++native_lane_routed_messages_;
  co_return co_await td::actor::ask(native_lane_owners_[owner], &ExtMessagePool::check_add_prepared_lane_single,
      std::move(item), priority, add_to_mempool, deadline, snapshot.move_as_ok(), native_lane_applied_generation_, td::Time::now());
}

td::actor::Task<ExtMessagePool::BatchCheckResult> ExtMessagePool::check_add_prepared_lane_batch(
    PreparedBatch prepared, int priority, bool add_to_mempool, td::Timestamp deadline,
    NativeAdmissionSnapshot snapshot, td::uint64 generation, double queued_at) {
  CHECK(is_native_lane_owner());
  native_lane_owner_queue_.observe(td::Time::now() - queued_at);
  ++native_lane_routed_batches_;
  native_lane_routed_messages_ += prepared.size();
  native_lane_max_batch_parents_ = std::max<td::uint64>(native_lane_max_batch_parents_, prepared.size());
  auto status = check_native_lane_fence(generation);
  if (status.is_ok() && (last_masterchain_state_.is_null() ||
      last_masterchain_state_->get_block_id() != snapshot.block_id ||
      RootHash{last_masterchain_state_->root_cell()->get_hash().bits()} != snapshot.state_root)) {
    status = td::Status::Error(ErrorCode::notready, "native pool routed admission snapshot changed; retry");
  }
  for (const auto &item : prepared) {
    if (status.is_error()) { break; }
    if (item.is_ok()) { status = check_native_lane_message(*item.ok().message_, generation); }
  }
  if (deadline && deadline.is_in_past()) {
    status = td::Status::Error(ErrorCode::timeout, "external message admission deadline expired");
  }
  if (status.is_error()) {
    BatchCheckResult result;
    for (std::size_t i = 0; i < prepared.size(); ++i) { result.statuses.push_back(ExternalMessageAdmissionResult::failure(status.clone())); }
    co_return result;
  }
  std::vector<td::BufferSlice> empty(prepared.size());
  co_return co_await check_add_external_messages_impl(std::move(empty), priority, add_to_mempool, deadline,
      td::optional<PreparedBatch>{std::move(prepared)}, snapshot, generation);
}

td::actor::Task<ExtMessagePool::CheckResult> ExtMessagePool::check_add_prepared_lane_single(
    PreparedExternalMessage prepared, int priority, bool add_to_mempool, td::Timestamp deadline,
    NativeAdmissionSnapshot snapshot, td::uint64 generation, double queued_at) {
  auto message = prepared.message_;
  PreparedBatch group;
  group.emplace_back(std::move(prepared));
  auto result = co_await check_add_prepared_lane_batch(std::move(group), priority, add_to_mempool, deadline,
      std::move(snapshot), generation, queued_at);
  CHECK(result.statuses.size() == 1);
  if (!result.statuses.front().accepted) {
    co_return td::Status::Error(result.statuses.front().error_code, result.statuses.front().error_message);
  }
  if (!result.checked_messages.empty()) { co_return std::move(result.checked_messages.front()); }
  auto [wait, allow] = td::actor::StartedTask<>::make_bridge();
  allow.set_value(td::Unit{});
  co_return CheckResult{.message = std::move(message), .wait_allow_broadcast = std::move(wait), .should_broadcast = false, .msg_seqno = {}, .native_admission = {}};
}

void ExtMessagePool::install_lane_callback(std::shared_ptr<InstalledCallback> installed) {
  CHECK(is_native_lane_owner());
  if (check_native_lane_fence().is_error()) { cancel_callback_delivery(installed); return; }
  const auto shard = installed->callback->shard;
  if (native_persistent_producer_enabled_ && installed->callback->native_streaming && !installed->callback->sync_only) {
    // The queued owner flush and owner_pending_sources completion barrier publish this view.
    auto &producer = ensure_native_producer(shard);
    td::actor::send_closure(producer.actor, &ExtMessagePool::install_native_producer_callback, std::move(installed));
    return;
  }
  restore_callback_native_cursor(installed);
  prefill_callback_native(installed, true);
  persist_callback_native_cursor(installed);
  if (installed->callback->sync_only) {
    const auto remaining = installed->callback->queue_capacity > installed->delivered_native_logical
        ? installed->callback->queue_capacity - installed->delivered_native_logical : 0;
    while (installed->pending_generic.size() > remaining) { installed->pending_generic.pop_back(); }
    installed->generic_selected = installed->pending_generic.size();
  }
  start_callback_pump(installed);
  if (!installed->callback->sync_only) {
    alarm_timestamp().relax(installed->callback->timeout);
    callbacks_.push_back(std::move(installed));
  }
}

void ExtMessagePool::deliver_lane_generic(td::Ref<ExtMessage> message, int priority) {
  CHECK(is_native_lane_owner());
  if (check_native_lane_fence().is_error()) { return; }
  if (auto lane = native_producer_lane({message->wc(), message->addr()})) {
    auto producer = native_producers_.find(lane.value());
    if (producer != native_producers_.end()) {
      td::actor::send_closure(producer->second.actor, &ExtMessagePool::deliver_native_producer_generic, message, priority);
    }
  }
  std::erase_if(callbacks_, [&](const auto &callback) {
    if (callback->callback->cancellation_token.check().is_error()) { cancel_callback_delivery(callback); return true; }
    if (shard_contains(callback->callback->shard, message->shard()) &&
        !std::binary_search(callback->callback->excluded_messages.begin(), callback->callback->excluded_messages.end(), message->hash())) {
      ++callback->generic_selected;
      begin_callback_epoch(callback);
      enqueue_callback_item(callback, {message, priority}, false);
      start_callback_pump(callback);
    }
    return false;
  });
}

void ExtMessagePool::prepare_stats_all(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
  if (!is_native_lane_router()) { promise.set_value(prepare_stats()); return; }
  collect_native_lane_stats(std::move(promise)).start().detach();
}

td::actor::Task<> ExtMessagePool::collect_native_lane_stats(
    td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
  using Stats = std::vector<std::pair<std::string, std::string>>;
  auto result = prepare_stats();
  std::vector<td::actor::StartedTask<Stats>> tasks;
  for (auto &owner : native_lane_owners_) {
    auto [task, reply] = td::actor::StartedTask<Stats>::make_bridge();
    td::actor::send_closure(owner, &ExtMessagePool::prepare_stats_all, std::move(reply));
    tasks.push_back(std::move(task));
  }
  auto replies = co_await td::actor::all_wrap(std::move(tasks));
  for (std::size_t index = 0; index < replies.size(); ++index) {
    if (replies[index].is_error()) { promise.set_error(replies[index].move_as_error()); co_return {}; }
    for (auto &[key, value] : replies[index].ok()) {
      result.emplace_back(PSTRING() << "native_pool.owner." << index << '.' << key, std::move(value));
    }
  }
  promise.set_value(std::move(result));
  co_return {};
}
}  // namespace ton::validator
