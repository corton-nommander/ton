/*
    This file is part of TON Blockchain Library.
    Copyright (c) 2026 TON contributors.
    Licensed under the GNU Lesser General Public License, version 2 or later.
*/
#include "ext-message-pool.hpp"
#include "ton/ton-io.hpp"
#include "td/utils/StringBuilder.h"

namespace ton::validator {
namespace {
td::uint64 producer_micros(double seconds) {
  return static_cast<td::uint64>(std::max(0.0, seconds) * 1e6);
}
void producer_peak(std::atomic<td::uint64> &counter, td::uint64 value) {
  auto previous = counter.load(std::memory_order_relaxed);
  while (previous < value && !counter.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {}
}
}  // namespace

td::optional<ShardIdFull> ExtMessagePool::native_producer_lane(const NativeAddress &source) const {
  if (!native_persistent_producer_enabled_ || native_producer_only_ || source.first != basechainId ||
      !native_payment_lane_depth_ || native_payment_lane_depth_.value() == 0 ||
      native_payment_lane_depth_.value() > 6) {
    return {};
  }
  return shard_prefix(extract_addr_prefix(source.first, source.second), native_payment_lane_depth_.value());
}

ExtMessagePool::NativeProducerEndpoint &ExtMessagePool::ensure_native_producer(ShardIdFull shard) {
  auto [it, inserted] = native_producers_.try_emplace(shard);
  if (inserted) {
    it->second.inbox = std::make_shared<NativeProducerInbox>();
    it->second.actor = td::actor::create_actor<ExtMessagePool>(
        PSTRING() << "native-producer-" << shard, shard, it->second.inbox, native_producer_telemetry_,
        native_transport_telemetry_, native_collator_queue_limit_);
  }
  return it->second;
}

void ExtMessagePool::mark_native_producer_source(const NativeAddress &source, bool invalidate) {
  auto lane = native_producer_lane(source);
  if (!lane) { return; }
  auto &fence = native_producer_fences_[source];
  if (!fence) { fence = std::make_shared<NativeProducerFence>(); }
  if (auto watermark = native_nonce_watermarks_.find(source); watermark != native_nonce_watermarks_.end()) {
    fence->canonical_next_nonce.store(watermark->second.observed_next_nonce, std::memory_order_release);
  }
  if (invalidate) { fence->invalidation.fetch_add(1, std::memory_order_release); }
  if (native_producer_dirty_sources_.insert(source).second) {
    ensure_native_producer(lane.value()).inbox->owner_pending_sources.fetch_add(1, std::memory_order_release);
  }
  if (!native_producer_flush_scheduled_) {
    native_producer_flush_scheduled_ = true;
    td::actor::send_closure_later(actor_id(this), &ExtMessagePool::flush_native_producer_sources);
  }
}

ExtMessagePool::NativePublishedSourcePtr ExtMessagePool::make_native_producer_source(const NativeAddress &source) {
  auto result = std::make_shared<NativePublishedSource>();
  result->source = source;
  auto fence = native_producer_fences_.find(source);
  CHECK(fence != native_producer_fences_.end());
  result->fence = fence->second;
  result->invalidation = result->fence->invalidation.load(std::memory_order_acquire);
  auto info = native_accounts_.find(source);
  if (info == native_accounts_.end() || !native_nonce_watermarks_.contains(source)) { return result; }
  result->messages.reserve(info->second.messages.size());
  for (const auto &[nonce, reservation] : info->second.messages) {
    if (!reservation.committed || !reservation.has_valid_interval(nonce)) { continue; }
    auto valid = [&](const MempoolMsg &msg) {
      return msg.message.not_null() && msg.in_mempool && msg.has_native_interval() &&
             msg.message->hash() == reservation.hash && msg.address() == source &&
             msg.native_nonce.value() == nonce && msg.native_nonce_count == reservation.logical_count &&
             msg.native_is_run == reservation.is_run && msg.native_payment_lane_depth == reservation.payment_lane_depth;
    };
    const MempoolMsg *message = nullptr;
    int priority = 0;
    if (reservation.mempool_link) {
      const auto &link = reservation.mempool_link.value();
      if (link.message && valid(*link.message) && link.id.hash == reservation.hash &&
          link.id.dst == link.message->message->shard()) {
        message = link.message.get();
        priority = link.priority;
      }
    }
    if (!message) {
      auto hash = ext_messages_hashes_.find(reservation.hash);
      if (hash == ext_messages_hashes_.end()) { continue; }
      auto tier = ext_msgs_.find(hash->second.first);
      if (tier == ext_msgs_.end()) { continue; }
      auto found = tier->second.ext_messages_.find(hash->second.second);
      if (!found || !valid(*found.value())) { continue; }
      message = found.value().get();
      priority = hash->second.first;
    }
    const auto &msg = *message;
    result->messages.push_back({msg.message, nonce, reservation.logical_count, priority,
                                msg.delete_at, msg.reactivate_at, msg.active});
    if (!result->maximum_priority || priority > result->maximum_priority.value()) { result->maximum_priority = priority; }
    if (!result->minimum_priority || priority < result->minimum_priority.value()) { result->minimum_priority = priority; }
  }
  return result;
}

void ExtMessagePool::flush_native_producer_sources() {
  native_producer_flush_scheduled_ = false;
  if (native_producer_dirty_sources_.empty()) { return; }
  const auto started = td::Timestamp::now().at();
  // The admission owner has one coalesced flush. Each producer inbox also has
  // at most one pending notification and one newest view per dirty source.
  std::map<ShardIdFull, std::vector<NativePublishedSourcePtr>> updates;
  auto end = native_producer_dirty_sources_.begin();
  for (std::size_t remaining = 256; remaining != 0 && end != native_producer_dirty_sources_.end(); --remaining, ++end) {
    const auto &source = *end;
    if (auto lane = native_producer_lane(source)) {
      auto view = make_native_producer_source(source);
      native_producer_telemetry_->published_sources.fetch_add(1, std::memory_order_relaxed);
      native_producer_telemetry_->published_messages.fetch_add(view->messages.size(), std::memory_order_relaxed);
      updates[lane.value()].push_back(std::move(view));
    }
  }
  native_producer_dirty_sources_.erase(native_producer_dirty_sources_.begin(), end);
  if (!native_producer_dirty_sources_.empty()) {
    native_producer_flush_scheduled_ = true;
    td::actor::send_closure_later(actor_id(this), &ExtMessagePool::flush_native_producer_sources);
  }
  for (auto &[lane, views] : updates) {
    auto &endpoint = ensure_native_producer(lane);
    bool notify = false;
    {
      std::lock_guard lock(endpoint.inbox->mutex);
      for (auto &view : views) {
        if (endpoint.inbox->latest.size() >= 65536 && !endpoint.inbox->latest.contains(view->source)) {
          // Keep only the owner's source key while a slow producer catches up;
          // never discard an invalidation or grow an unbounded view mailbox.
          native_producer_dirty_sources_.insert(view->source);
          continue;
        }
        auto [it, inserted] = endpoint.inbox->latest.try_emplace(view->source, view);
        if (!inserted) {
          it->second = std::move(view);
          native_producer_telemetry_->replacements_coalesced.fetch_add(1, std::memory_order_relaxed);
        }
        CHECK(endpoint.inbox->owner_pending_sources.fetch_sub(1, std::memory_order_acq_rel) != 0);
      }
      producer_peak(native_producer_telemetry_->inbox_peak_sources, endpoint.inbox->latest.size());
      if (!endpoint.inbox->scheduled) {
        endpoint.inbox->scheduled = notify = true;
        endpoint.inbox->enqueued_at = td::Timestamp::now().at();
      }
    }
    if (notify) { td::actor::send_closure(endpoint.actor, &ExtMessagePool::consume_native_producer_updates); }
  }
  native_producer_telemetry_->publications.fetch_add(1, std::memory_order_relaxed);
  native_producer_telemetry_->publication_us.fetch_add(
      producer_micros(td::Timestamp::now().at() - started), std::memory_order_relaxed);
  if (!native_producer_dirty_sources_.empty() && !native_producer_flush_scheduled_) {
    native_producer_flush_scheduled_ = true;
    td::actor::send_closure_later(actor_id(this), &ExtMessagePool::flush_native_producer_sources);
  }
}

void ExtMessagePool::apply_native_producer_update(NativePublishedSourcePtr view) {
  CHECK(native_producer_only_);
  auto previous = native_producer_sources_.find(view->source);
  const auto old_priority = previous == native_producer_sources_.end() ? td::optional<int>{}
                                                                     : previous->second->maximum_priority;
  if (previous != native_producer_sources_.end() &&
      previous->second->minimum_priority != previous->second->maximum_priority) {
    --native_producer_mixed_priority_sources_;
  }
  if (view->minimum_priority != view->maximum_priority) { ++native_producer_mixed_priority_sources_; }
  if (old_priority != view->maximum_priority) {
    if (old_priority) {
      auto &index = native_producer_index_.at(old_priority.value());
      index = index.erase(view->source);
      if (index.empty()) { native_producer_index_.erase(old_priority.value()); }
    }
    if (view->maximum_priority) {
      auto &index = native_producer_index_[view->maximum_priority.value()];
      index = index.insert(view->source, td::Unit{});
    }
    native_producer_telemetry_->index_updates.fetch_add(1, std::memory_order_relaxed);
  }
  // Reactivation tokens are cheap, advisory and checked against the newest
  // source view. Bound them by replacing all old tokens for this source below
  // in consume_native_producer_updates rather than accumulating retry history.
  if (view->messages.empty()) {
    native_producer_sources_.erase(view->source);
  } else {
    native_producer_sources_[view->source] = std::move(view);
  }
}

void ExtMessagePool::consume_native_producer_updates() {
  CHECK(native_producer_only_);
  std::map<NativeAddress, NativePublishedSourcePtr> updates;
  bool more = false;
  {
    std::lock_guard lock(native_producer_inbox_->mutex);
    for (std::size_t remaining = 256; remaining != 0 && !native_producer_inbox_->latest.empty(); --remaining) {
      updates.insert(native_producer_inbox_->latest.extract(native_producer_inbox_->latest.begin()));
    }
    if (!updates.empty()) {
      native_producer_telemetry_->inbox_wait_us.fetch_add(
          producer_micros(td::Timestamp::now().at() - native_producer_inbox_->enqueued_at), std::memory_order_relaxed);
    }
    more = !native_producer_inbox_->latest.empty();
    native_producer_inbox_->scheduled = more;
  }
  if (more) { td::actor::send_closure_later(actor_id(this), &ExtMessagePool::consume_native_producer_updates); }
  if (updates.empty()) { return; }
  const bool was_uniform = native_producer_index_.size() <= 1 && native_producer_mixed_priority_sources_ == 0;
  std::set<NativeAddress> changed;
  for (auto &[source, view] : updates) {
    changed.insert(source);
    apply_native_producer_update(std::move(view));
  }
  if (was_uniform && (native_producer_index_.size() > 1 || native_producer_mixed_priority_sources_ != 0)) {
    for (auto &callback : callbacks_) { callback->native_scheduler_rebuild = true; }
  }
  // Only one earliest outstanding reactivation per source is retained.
  // The source deadline map is rebuilt from changed views; unchanged sources
  // keep their existing event (see the reverse map below).
  for (const auto &source : changed) {
    if (auto event = native_producer_reactivation_by_source_.find(source);
        event != native_producer_reactivation_by_source_.end()) {
      native_producer_reactivations_.erase(event->second);
      native_producer_reactivation_by_source_.erase(event);
    }
    auto view = native_producer_sources_.find(source);
    if (view == native_producer_sources_.end()) { continue; }
    td::Timestamp earliest;
    for (const auto &message : view->second->messages) {
      if (!message.active && !message.reactivate_at.is_in_past()) { earliest.relax(message.reactivate_at); }
    }
    if (earliest) {
      native_producer_reactivation_by_source_[source] = native_producer_reactivations_.emplace(earliest, source);
      alarm_timestamp().relax(earliest);
    }
  }
  wake_native_callbacks(&changed);
}

void ExtMessagePool::reset_native_producers() {
  if (native_producer_only_ || !native_persistent_producer_enabled_) { return; }
  for (auto &[_, fence] : native_producer_fences_) { fence->invalidation.fetch_add(1, std::memory_order_release); }
  native_producers_.clear();
  native_producer_dirty_sources_.clear();
  // Rebuild membership once on a supported topology transition, never per
  // candidate. Old callback leases are closed by the retired producer actors.
  for (const auto &[source, _] : native_accounts_) { mark_native_producer_source(source); }
}

void ExtMessagePool::install_native_producer_callback(std::shared_ptr<InstalledCallback> callback) {
  CHECK(native_producer_only_ && callback->callback->shard == native_producer_shard_.value());
  const auto started = td::Timestamp::now().at();
  consume_native_producer_updates();
  if (callback->callback->cancellation_token.check().is_error() ||
      (callback->callback->timeout && callback->callback->timeout.is_in_past())) {
    cancel_callback_delivery(callback);
    return;
  }
  restore_callback_native_cursor(callback);
  prefill_callback_native(callback, true);
  persist_callback_native_cursor(callback);
  start_callback_pump(callback);
  alarm_timestamp().relax(callback->callback->timeout);
  callbacks_.push_back(std::move(callback));
  native_producer_telemetry_->installs.fetch_add(1, std::memory_order_relaxed);
  native_producer_telemetry_->install_us.fetch_add(
      producer_micros(td::Timestamp::now().at() - started), std::memory_order_relaxed);
}

void ExtMessagePool::probe_published_native_source(const std::shared_ptr<InstalledCallback> &callback,
    const NativeAddress &source, CallbackNativeSource &state, NativeQueueCounters &counters, bool enqueue_ready) {
  auto found = native_producer_sources_.find(source);
  if (found == native_producer_sources_.end()) { return; }
  const auto &view = *found->second;
  native_producer_telemetry_->source_probes.fetch_add(1, std::memory_order_relaxed);
  if (view.invalidation != view.fence->invalidation.load(std::memory_order_acquire)) {
    native_producer_telemetry_->invalidated_probes.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto first_nonce = callback->effective_native_nonce_floor(
      source, view.fence->canonical_next_nonce.load(std::memory_order_acquire));
  state.next_nonce = std::max(state.next_nonce, first_nonce);
  auto it = std::lower_bound(view.messages.begin(), view.messages.end(), state.next_nonce,
                             [](const auto &msg, td::uint64 nonce) { return msg.nonce < nonce; });
  for (; it != view.messages.end(); ++it) {
    if (it->nonce != state.next_nonce) { ++counters.head_gaps; ++counters.head_missing_nonce; return; }
    ++counters.scanned;
    const auto hash = it->message->hash();
    bool skip = false;
    if (std::binary_search(callback->callback->excluded_messages.begin(),
                           callback->callback->excluded_messages.end(), hash)) { ++counters.excluded; skip = true; }
    else if (callback->delivered_native.contains(hash)) { ++counters.already_delivered; skip = true; }
    if (skip) {
      if (state.next_nonce > std::numeric_limits<td::uint64>::max() - it->logical_count) { return; }
      state.next_nonce += it->logical_count;
      continue;
    }
    if (it->delete_at.is_in_past()) { ++counters.expired; return; }
    if (!it->active && !it->reactivate_at.is_in_past()) {
      ++counters.inactive;
      auto event = native_producer_reactivation_by_source_.find(source);
      if (event == native_producer_reactivation_by_source_.end() || it->reactivate_at < event->second->first) {
        if (event != native_producer_reactivation_by_source_.end()) {
          native_producer_reactivations_.erase(event->second);
        }
        native_producer_reactivation_by_source_[source] =
            native_producer_reactivations_.emplace(it->reactivate_at, source);
      }
      alarm_timestamp().relax(it->reactivate_at);
      return;
    }
    ++counters.active;
    if (!state.ready_counted) { state.ready_counted = true; ++counters.ready_sources; }
    state.next = NativeQueueItem{it->message, it->priority, source, it->nonce, it->logical_count};
    if (enqueue_ready) { enqueue_callback_native_source(callback->native_scheduler, source, state); }
    return;
  }
  if (state.next_nonce > first_nonce) { ++counters.speculative_exhausted; }
  else { ++counters.head_gaps; ++counters.head_missing_nonce; }
}

void ExtMessagePool::deliver_native_producer_generic(td::Ref<ExtMessage> message, int priority) {
  CHECK(native_producer_only_);
  std::erase_if(callbacks_, [&](const auto &callback) {
    if (callback->callback->cancellation_token.check().is_error()) { cancel_callback_delivery(callback); return true; }
    if (!std::binary_search(callback->callback->excluded_messages.begin(),
                            callback->callback->excluded_messages.end(), message->hash())) {
      ++callback->generic_selected;
      begin_callback_epoch(callback);
      enqueue_callback_item(callback, {message, priority}, false);
      start_callback_pump(callback);
    }
    return false;
  });
}

void ExtMessagePool::tear_down() {
  for (auto &callback : callbacks_) { cancel_callback_delivery(callback); }
  callbacks_.clear();
}
}  // namespace ton::validator
