/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include <algorithm>
#include <deque>
#include <map>
#include <utility>
#include <vector>

#include "interfaces/liteserver.h"
#include "interfaces/validator-manager.h"
#include "td/actor/coro_utils.h"
#include "td/actor/SharedFuture.h"
#include "td/utils/LRUCache.h"

namespace ton::validator {

class LiteServerCacheImpl : public LiteServerCache {
 public:
  explicit LiteServerCacheImpl(td::actor::ActorId<ValidatorManager> manager) : manager_(std::move(manager)) {
  }

  void start_up() override {
    cache_stats_at_ = td::Timestamp::in(cache_stats_period);
    schedule_alarm();
  }

  void alarm() override {
    if (cache_stats_at_.is_in_past()) {
      if (queries_cnt_ > 0 || !send_message_cache_.empty() || native_send_message_batches_ > 0 ||
          native_send_message_expired_ > 0 || native_send_message_overloaded_ > 0 || native_send_message_failed_ > 0) {
        LOG(WARNING) << "LS Cache stats: " << queries_cnt_ << " queries, " << queries_hit_cnt_ << " hits; "
                     << cache_.size() << " entries, size=" << total_size_ << "/" << MAX_CACHE_SIZE << ";   "
                     << send_message_cache_.size() << " different sendMessage queries, " << send_message_error_cnt_
                     << " duplicates; native microbatches=" << native_send_message_batches_ << " items="
                     << native_send_message_items_ << " max_batch=" << native_send_message_max_batch_
                     << " queue_peak=" << native_send_message_queue_peak_ << " expired=" << native_send_message_expired_
                     << " overloaded=" << native_send_message_overloaded_ << " failed=" << native_send_message_failed_;
      }
      queries_cnt_ = 0;
      queries_hit_cnt_ = 0;
      send_message_error_cnt_ = 0;
      native_send_message_batches_ = 0;
      native_send_message_items_ = 0;
      native_send_message_max_batch_ = 0;
      native_send_message_queue_peak_ = native_send_messages_.size();
      native_send_message_expired_ = 0;
      native_send_message_overloaded_ = 0;
      native_send_message_failed_ = 0;
      cache_stats_at_ = td::Timestamp::in(cache_stats_period);
    }
    expire_native_send_messages();
    if (!native_send_message_batch_active_ && !native_send_messages_.empty() &&
        (!native_send_message_flush_at_ || native_send_message_flush_at_.is_in_past())) {
      flush_native_send_messages();
    }
    schedule_alarm();
  }

  void lookup(td::Bits256 key, td::Promise<td::BufferSlice> promise) override {
    ++queries_cnt_;
    auto it = cache_.find(key);
    if (it == cache_.end()) {
      promise.set_error(td::Status::Error("not found"));
      return;
    }
    ++queries_hit_cnt_;
    auto entry = it->second.get();
    entry->remove();
    lru_.put(entry);
    promise.set_value(entry->value_.clone());
  }

  void update(td::Bits256 key, td::BufferSlice value) override {
    std::unique_ptr<CacheEntry> &entry = cache_[key];
    if (entry == nullptr) {
      entry = std::make_unique<CacheEntry>(key, std::move(value));
    } else {
      total_size_ -= entry->size();
      entry->value_ = std::move(value);
      entry->remove();
    }
    lru_.put(entry.get());
    total_size_ += entry->size();

    while (total_size_ > MAX_CACHE_SIZE) {
      auto to_remove = (CacheEntry *)lru_.get();
      CHECK(to_remove);
      total_size_ -= to_remove->size();
      to_remove->remove();
      cache_.erase(to_remove->key_);
    }
  }

  void process_send_message(td::Bits256 key, td::uint64 owner, td::Promise<td::Unit> promise) override {
    if (send_message_cache_.contains(key)) {
      ++send_message_error_cnt_;
      promise.set_error(td::Status::Error(ErrorCode::notready, "identical sendMessage query is still in flight"));
      return;
    }
    send_message_cache_.put(key, owner);
    promise.set_value(td::Unit{});
  }

  void drop_send_message_from_cache(td::Bits256 key, td::uint64 owner) override {
    auto current_owner = send_message_cache_.get_if_exists(key, false);
    if (current_owner != nullptr && *current_owner == owner) {
      send_message_cache_.erase(key);
    }
  }

  void process_native_send_message(td::BufferSlice data, td::Timestamp deadline,
                                   td::Promise<td::Unit> promise) override {
    if (!deadline || deadline.is_in_past()) {
      ++native_send_message_expired_;
      promise.set_error(td::Status::Error(ErrorCode::timeout, "native sendMessage admission deadline expired"));
      return;
    }
    if (data.size() > native_send_message_coalescing_max_message_bytes ||
        native_send_messages_.size() + native_send_message_inflight_items_ >= max_native_pending_messages ||
        native_send_message_pending_bytes_ + native_send_message_inflight_bytes_ + data.size() >
            max_native_pending_bytes) {
      ++native_send_message_overloaded_;
      promise.set_error(td::Status::Error(ErrorCode::notready, "native sendMessage admission queue is full"));
      return;
    }
    native_send_message_pending_bytes_ += data.size();
    native_send_messages_.push_back({std::move(data), deadline, std::move(promise)});
    native_send_message_earliest_deadline_.relax(deadline);
    native_send_message_queue_peak_ = std::max(native_send_message_queue_peak_, native_send_messages_.size());

    if (!native_send_message_batch_active_) {
      if (native_send_messages_.size() >= max_native_send_message_batch) {
        flush_native_send_messages();
      } else {
        native_send_message_flush_at_.relax(td::Timestamp::in(native_send_message_coalescing_delay));
        schedule_alarm();
      }
    } else {
      // A single in-flight admission batch preserves the pool's deterministic
      // native reservation order. The next batch launches immediately when it
      // completes, while this alarm still expires abandoned requests on time.
      schedule_alarm();
    }
  }

 private:
  struct PendingNativeSendMessage {
    td::BufferSlice data;
    td::Timestamp deadline;
    td::Promise<td::Unit> promise;
  };

  static constexpr size_t MAX_CACHE_SIZE = 64 << 20;
  static constexpr size_t MAX_MSG_CACHE_SIZE = 1 << 17;
  static constexpr std::size_t max_native_send_message_batch = 256;
  static constexpr std::size_t max_native_send_message_batch_bytes = 8 << 20;
  static constexpr std::size_t max_native_pending_messages = 4096;
  static constexpr std::size_t max_native_pending_bytes = 8 << 20;
  static constexpr double native_send_message_coalescing_delay = 0.001;
  // Requests from the same 1 ms collection window differ only by actor
  // scheduling noise. Do not let an old queued head consume a later request's
  // whole deadline: it receives at most this conservative early deadline.
  static constexpr double native_send_message_deadline_group_slack = 0.010;
  static constexpr double cache_stats_period = 60.0;

  struct CacheEntry : public td::ListNode {
    explicit CacheEntry(td::Bits256 key, td::BufferSlice value) : key_(key), value_(std::move(value)) {
    }
    td::Bits256 key_;
    td::BufferSlice value_;

    size_t size() const {
      return value_.size() + 32 * 2;
    }
  };

  std::map<td::Bits256, std::unique_ptr<CacheEntry>> cache_;
  td::ListNode lru_;
  size_t total_size_ = 0;

  void schedule_alarm() {
    auto next = cache_stats_at_;
    if (!native_send_messages_.empty()) {
      if (!native_send_message_batch_active_) {
        next.relax(native_send_message_flush_at_);
      }
      next.relax(native_send_message_earliest_deadline_);
    }
    alarm_timestamp() = next;
  }

  void refresh_native_send_message_earliest_deadline() {
    native_send_message_earliest_deadline_ = td::Timestamp::never();
    for (const auto &entry : native_send_messages_) {
      native_send_message_earliest_deadline_.relax(entry.deadline);
    }
  }

  void expire_native_send_messages() {
    bool erased = false;
    auto it = native_send_messages_.begin();
    while (it != native_send_messages_.end()) {
      if (!it->deadline || !it->deadline.is_in_past()) {
        ++it;
        continue;
      }
      native_send_message_pending_bytes_ -= it->data.size();
      it->promise.set_error(td::Status::Error(ErrorCode::timeout, "native sendMessage admission deadline expired"));
      it = native_send_messages_.erase(it);
      ++native_send_message_expired_;
      erased = true;
    }
    if (native_send_messages_.empty()) {
      native_send_message_flush_at_ = td::Timestamp::never();
      native_send_message_earliest_deadline_ = td::Timestamp::never();
    } else if (erased) {
      refresh_native_send_message_earliest_deadline();
    }
  }

  void flush_native_send_messages() {
    CHECK(!native_send_message_batch_active_);
    expire_native_send_messages();
    if (native_send_messages_.empty()) {
      schedule_alarm();
      return;
    }

    std::vector<PendingNativeSendMessage> pending;
    std::vector<td::BufferSlice> messages;
    pending.reserve(max_native_send_message_batch);
    messages.reserve(max_native_send_message_batch);
    td::Timestamp deadline;
    std::size_t batch_bytes = 0;
    while (!native_send_messages_.empty() && pending.size() < max_native_send_message_batch) {
      auto &entry = native_send_messages_.front();
      if (!pending.empty() && batch_bytes + entry.data.size() > max_native_send_message_batch_bytes) {
        break;
      }
      if (!pending.empty() && entry.deadline.at() - deadline.at() > native_send_message_deadline_group_slack) {
        break;
      }
      batch_bytes += entry.data.size();
      deadline.relax(entry.deadline);
      native_send_message_pending_bytes_ -= entry.data.size();
      messages.push_back(std::move(entry.data));
      pending.push_back(std::move(entry));
      native_send_messages_.pop_front();
    }
    CHECK(!pending.empty());
    CHECK(deadline);
    refresh_native_send_message_earliest_deadline();
    native_send_message_flush_at_ = td::Timestamp::never();
    native_send_message_batch_active_ = true;
    native_send_message_inflight_items_ = pending.size();
    native_send_message_inflight_bytes_ = batch_bytes;
    ++native_send_message_batches_;
    native_send_message_items_ += pending.size();
    native_send_message_max_batch_ = std::max(native_send_message_max_batch_, pending.size());

    auto run = [](td::actor::ActorId<LiteServerCacheImpl> self,
                  td::actor::ActorId<ValidatorManager> manager,
                  std::vector<PendingNativeSendMessage> pending,
                  std::vector<td::BufferSlice> messages, td::Timestamp deadline) -> td::actor::Task<> {
      // The manager and pool carry the same absolute deadline and reject it
      // before synchronous reservation. Do not wrap this in
      // await_with_timeout: that helper detaches the underlying task, which
      // would let a later batch overlap this lane's native reservations.
      auto result = co_await td::actor::ask(manager, &ValidatorManager::new_external_message_batch_query_until,
                                             std::move(messages), deadline)
                        .wrap();
      td::actor::send_closure(self, &LiteServerCacheImpl::finish_native_send_message_batch, std::move(pending),
                              std::move(result));
      co_return td::Unit{};
    };
    native_send_message_batch_task_ =
        run(actor_id(this), manager_, std::move(pending), std::move(messages), deadline).start();
    schedule_alarm();
  }

  void finish_native_send_message_batch(std::vector<PendingNativeSendMessage> pending,
                                        td::Result<ExternalMessageAdmissionResults> result) {
    CHECK(native_send_message_batch_active_);
    CHECK(native_send_message_inflight_items_ == pending.size());
    native_send_message_batch_active_ = false;
    native_send_message_batch_task_ = {};
    native_send_message_inflight_items_ = 0;
    native_send_message_inflight_bytes_ = 0;
    if (result.is_error()) {
      auto error = result.move_as_error();
      for (auto &entry : pending) {
        entry.promise.set_error(error.clone());
      }
      native_send_message_failed_ += pending.size();
    } else {
      auto admissions = result.move_as_ok();
      if (admissions.size() != pending.size()) {
        auto error = td::Status::Error(ErrorCode::notready,
                                       "native sendMessage admission returned an incomplete batch result");
        for (auto &entry : pending) {
          entry.promise.set_error(error.clone());
        }
        native_send_message_failed_ += pending.size();
      } else {
        for (std::size_t i = 0; i < pending.size(); ++i) {
          if (admissions[i].accepted) {
            pending[i].promise.set_value(td::Unit{});
          } else {
            ++native_send_message_failed_;
            pending[i].promise.set_error(
                td::Status::Error(admissions[i].error_code, std::move(admissions[i].error_message)));
          }
        }
      }
    }

    expire_native_send_messages();
    // Requests that accumulated behind the previous bounded batch have
    // already spent their coalescing interval, so drain the next prefix now.
    if (!native_send_messages_.empty()) {
      flush_native_send_messages();
    } else {
      schedule_alarm();
    }
  }

  size_t queries_cnt_ = 0, queries_hit_cnt_ = 0;

  td::LRUCache<td::Bits256, td::uint64> send_message_cache_{MAX_MSG_CACHE_SIZE};
  size_t send_message_error_cnt_ = 0;

  td::actor::ActorId<ValidatorManager> manager_;
  std::deque<PendingNativeSendMessage> native_send_messages_;
  std::size_t native_send_message_pending_bytes_ = 0;
  std::size_t native_send_message_inflight_items_ = 0;
  std::size_t native_send_message_inflight_bytes_ = 0;
  td::Timestamp native_send_message_flush_at_;
  td::Timestamp native_send_message_earliest_deadline_;
  td::Timestamp cache_stats_at_;
  bool native_send_message_batch_active_ = false;
  td::actor::StartedTask<> native_send_message_batch_task_;
  std::size_t native_send_message_batches_ = 0;
  std::size_t native_send_message_items_ = 0;
  std::size_t native_send_message_max_batch_ = 0;
  std::size_t native_send_message_queue_peak_ = 0;
  std::size_t native_send_message_expired_ = 0;
  std::size_t native_send_message_overloaded_ = 0;
  std::size_t native_send_message_failed_ = 0;
};

}  // namespace ton::validator
