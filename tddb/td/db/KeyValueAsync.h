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

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include <utility>
#include <vector>

#include "td/actor/PromiseFuture.h"
#include "td/actor/actor.h"
#include "td/actor/coro_utils.h"
#include "td/db/KeyValue.h"

namespace td {

template <class KeyT, class ValueT>
class KeyValueActor;

template <class KeyT, class ValueT>
class KeyValueAsync {
 public:
  using ActorType = KeyValueActor<KeyT, ValueT>;
  using SetEntry = std::pair<KeyT, ValueT>;
  using SetBatch = std::vector<SetEntry>;
  struct GetResult {
    KeyValue::GetStatus status;
    ValueT value;
  };
  KeyValueAsync(std::shared_ptr<KeyValue> key_value);
  void get(KeyT key, Promise<GetResult> promise) const;
  void set(KeyT key, ValueT value, Promise<Unit> promise, double sync_delay = 0) const;
  // Writes all entries in one durable write batch. Existing asynchronous writes
  // are flushed first, so a failed batch cannot expose only a suffix/prefix of
  // this batch through the KeyValueActor.
  void set_many(SetBatch entries, Promise<Unit> promise) const;
  void erase(KeyT key, Promise<Unit> promise, double sync_delay = 0) const;

  actor::Task<GetResult> get(KeyT key) const;
  actor::Task<> set(KeyT key, ValueT value, double sync_delay = 0) const;
  actor::Task<> set_many(SetBatch entries) const;
  actor::Task<> erase(KeyT key, double sync_delay = 0) const;

  actor::Task<> close() const;

  KeyValueAsync();
  KeyValueAsync(KeyValueAsync &&);
  KeyValueAsync &operator=(KeyValueAsync &&);
  ~KeyValueAsync();

 private:
  actor::ActorOwn<ActorType> actor_;
};

template <class KeyT, class ValueT>
class KeyValueActor : public actor::Actor {
 public:
  KeyValueActor(std::shared_ptr<KeyValue> key_value) : key_value_(std::move(key_value)) {
  }

  void get(KeyT key, Promise<typename KeyValueAsync<KeyT, ValueT>::GetResult> promise) {
    if (!key_value_) {
      promise.set_error(Status::Error(/* cancelled */ 653, "db is closed"));
      return;
    }
    std::string value;
    auto r_status = key_value_->get(as_slice(key), value);
    if (r_status.is_error()) {
      promise.set_error(r_status.move_as_error());
      return;
    }
    typename KeyValueAsync<KeyT, ValueT>::GetResult result;
    result.status = r_status.move_as_ok();
    if (result.status == KeyValue::GetStatus::Ok) {
      result.value = ValueT(std::move(value));
    }
    promise.set_value(std::move(result));
  }
  void set(KeyT key, ValueT value, double sync_delay, Promise<Unit> promise) {
    if (!key_value_) {
      promise.set_error(Status::Error(/* cancelled */ 653, "db is closed"));
      return;
    }
    TRY_STATUS_PROMISE(promise, schedule_sync(sync_delay));
    TRY_STATUS_PROMISE(promise, key_value_->set(as_slice(key), as_slice(value)));
    pending_promises_.push_back(std::move(promise));
  }
  void set_many(typename KeyValueAsync<KeyT, ValueT>::SetBatch entries, Promise<Unit> promise) {
    if (!key_value_) {
      promise.set_error(Status::Error(/* cancelled */ 653, "db is closed"));
      return;
    }
    if (entries.empty()) {
      promise.set_value(Unit{});
      return;
    }

    // A queued single-key write may still own the current transaction. Commit it
    // before opening this exclusive batch, so a failure below can be aborted
    // without affecting an unrelated write.
    auto pending_status = sync();
    if (pending_status.is_error()) {
      promise.set_error(std::move(pending_status));
      return;
    }

    auto status = key_value_->begin_write_batch();
    if (status.is_error()) {
      promise.set_error(std::move(status));
      return;
    }
    for (auto& [key, value] : entries) {
      status = key_value_->set(as_slice(key), as_slice(value));
      if (status.is_error()) {
        key_value_->abort_write_batch().ignore();
        promise.set_error(std::move(status));
        return;
      }
    }
    promise.set_result(key_value_->commit_write_batch());
  }
  void erase(KeyT key, double sync_delay, Promise<Unit> promise) {
    if (!key_value_) {
      promise.set_error(Status::Error(/* cancelled */ 653, "db is closed"));
      return;
    }
    TRY_STATUS_PROMISE(promise, schedule_sync(sync_delay));
    TRY_STATUS_PROMISE(promise, key_value_->erase(as_slice(key)));
    pending_promises_.push_back(std::move(promise));
  }
  void close(Promise<Unit> promise) {
    if (!key_value_) {
      promise.set_value(Unit{});
      return;
    }
    sync();
    key_value_ = {};
    promise.set_value(Unit{});
  }

 private:
  std::shared_ptr<KeyValue> key_value_;
  std::vector<Promise<Unit>> pending_promises_;
  bool need_sync_ = false;
  bool sync_active_ = false;

  void tear_down() override {
    sync();
  }
  Status sync() {
    if (!need_sync_) {
      return Status::OK();
    }
    need_sync_ = false;
    sync_active_ = false;
    alarm_timestamp() = Timestamp::never();
    auto status = key_value_->commit_transaction();
    for (auto &promise : pending_promises_) {
      promise.set_result(status.clone());
    }
    pending_promises_.clear();
    return status;
  }
  Status schedule_sync(double sync_delay) {
    if (!need_sync_) {
      TRY_STATUS(key_value_->begin_transaction());
      need_sync_ = true;
    }

    if (!sync_active_) {
      if (sync_delay == 0) {
        send_sync();
      } else {
        alarm_timestamp().relax(Timestamp::in(sync_delay));
      }
    }
    return Status::OK();
  }
  void alarm() override {
    if (need_sync_ && !sync_active_) {
      send_sync();
    }
  }
  void send_sync() {
    sync_active_ = true;
    alarm_timestamp() = Timestamp::never();
    send_closure(actor_id(this), &KeyValueActor<KeyT, ValueT>::sync);
  }
};

template <class KeyT, class ValueT>
KeyValueAsync<KeyT, ValueT>::KeyValueAsync() = default;
template <class KeyT, class ValueT>
KeyValueAsync<KeyT, ValueT>::KeyValueAsync(KeyValueAsync &&) = default;
template <class KeyT, class ValueT>
KeyValueAsync<KeyT, ValueT> &KeyValueAsync<KeyT, ValueT>::operator=(KeyValueAsync &&) = default;
template <class KeyT, class ValueT>
KeyValueAsync<KeyT, ValueT>::~KeyValueAsync() = default;

template <class KeyT, class ValueT>
KeyValueAsync<KeyT, ValueT>::KeyValueAsync(std::shared_ptr<KeyValue> key_value) {
  actor_ = actor::create_actor<ActorType>("KeyValueActor", std::move(key_value));
}
template <class KeyT, class ValueT>
void KeyValueAsync<KeyT, ValueT>::get(KeyT key, Promise<GetResult> promise) const {
  send_closure_later(actor_, &ActorType::get, std::move(key), std::move(promise));
}
template <class KeyT, class ValueT>
void KeyValueAsync<KeyT, ValueT>::set(KeyT key, ValueT value, Promise<Unit> promise, double sync_delay) const {
  send_closure_later(actor_, &ActorType::set, std::move(key), std::move(value), sync_delay, std::move(promise));
}
template <class KeyT, class ValueT>
void KeyValueAsync<KeyT, ValueT>::set_many(SetBatch entries, Promise<Unit> promise) const {
  send_closure_later(actor_, &ActorType::set_many, std::move(entries), std::move(promise));
}
template <class KeyT, class ValueT>
void KeyValueAsync<KeyT, ValueT>::erase(KeyT key, Promise<Unit> promise, double sync_delay) const {
  send_closure_later(actor_, &ActorType::erase, std::move(key), sync_delay, std::move(promise));
}

template <class KeyT, class ValueT>
actor::Task<typename KeyValueAsync<KeyT, ValueT>::GetResult> KeyValueAsync<KeyT, ValueT>::get(KeyT key) const {
  co_return co_await actor::ask(actor_, &ActorType::get, std::move(key));
}

template <class KeyT, class ValueT>
actor::Task<> KeyValueAsync<KeyT, ValueT>::set(KeyT key, ValueT value, double sync_delay) const {
  co_return co_await actor::ask(actor_, &ActorType::set, std::move(key), std::move(value), sync_delay);
}

template <class KeyT, class ValueT>
actor::Task<> KeyValueAsync<KeyT, ValueT>::set_many(SetBatch entries) const {
  co_return co_await actor::ask(actor_, &ActorType::set_many, std::move(entries));
}

template <class KeyT, class ValueT>
actor::Task<> KeyValueAsync<KeyT, ValueT>::erase(KeyT key, double sync_delay) const {
  co_return co_await actor::ask(actor_, &ActorType::erase, std::move(key), sync_delay);
}

template <class KeyT, class ValueT>
actor::Task<> KeyValueAsync<KeyT, ValueT>::close() const {
  co_return co_await actor::ask(actor_, &ActorType::close);
}

}  // namespace td
