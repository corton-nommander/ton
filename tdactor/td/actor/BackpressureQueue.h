/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <algorithm>
#include <deque>
#include <limits>
#include <optional>
#include <queue>
#include <vector>

#include "td/actor/actor.h"
#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/actor/SharedFuture.h"

namespace td::actor {

template <typename T>
class BackpressureQueueActor : public Actor {
 public:
  explicit BackpressureQueueActor(size_t capacity) : capacity_(capacity) {
    CHECK(capacity > 0);
  }

  // Push an item. If block=true and queue is full, suspends until space is available.
  // If block=false and queue is full, returns false immediately.
  // Returns false if closed.
  Task<bool> push(T item, bool block = true) {
    while (true) {
      if (closed_) {
        co_return false;
      }
      if (items_.size() < capacity_) {
        items_.push(std::move(item));
        wake_pop_waiters(1);
        co_return true;
      }
      if (!block) {
        co_return false;
      }
      auto [task, promise] = StartedTask<td::Unit>::make_bridge();
      push_waiters_.push_back(std::move(promise));
      auto result = co_await std::move(task).wrap();
      if (result.is_error()) {
        co_return false;
      }
    }
  }

  // Push a FIFO batch. The hard queue capacity is preserved: when block=true,
  // this may suspend after a prefix has been inserted and resumes as consumers
  // make space. Returns the exact number inserted if the queue is closed.
  Task<size_t> push_many(std::vector<T> items, size_t occupancy_limit, bool block = true) {
    auto effective_capacity = std::min(capacity_, occupancy_limit);
    CHECK(effective_capacity > 0);
    size_t pushed = 0;
    while (pushed < items.size()) {
      if (closed_) {
        co_return pushed;
      }
      auto available = items_.size() < effective_capacity ? effective_capacity - items_.size() : 0;
      if (available != 0) {
        auto count = std::min(available, items.size() - pushed);
        for (size_t i = 0; i < count; ++i) {
          items_.push(std::move(items[pushed++]));
        }
        wake_pop_waiters(count);
        continue;
      }
      if (!block) {
        co_return pushed;
      }
      auto [task, promise] = StartedTask<td::Unit>::make_bridge();
      push_waiters_.push_back(std::move(promise));
      auto result = co_await std::move(task).wrap();
      if (result.is_error()) {
        co_return pushed;
      }
    }
    co_return pushed;
  }

  // Pop an item. If block=true and queue is empty, suspends until an item arrives.
  // If block=false and queue is empty, returns error immediately.
  // Returns error if closed and empty.
  Task<T> pop(bool block = true) {
    while (true) {
      if (!items_.empty()) {
        auto item = std::move(items_.front());
        items_.pop();
        wake_push_waiters(1);
        co_return std::move(item);
      }
      if (closed_ || !block) {
        co_return td::Status::Error("BackpressureQueue is empty");
      }
      auto [task, promise] = StartedTask<td::Unit>::make_bridge();
      pop_waiters_.push_back(PopWaiter{++next_pop_waiter_id_, std::move(promise)});
      auto result = co_await std::move(task).wrap();
      if (result.is_error()) {
        if (!items_.empty()) {
          auto item = std::move(items_.front());
          items_.pop();
          wake_push_waiters(1);
          co_return std::move(item);
        }
        co_return td::Status::Error("BackpressureQueue is closed");
      }
    }
  }

  // Pop up to max_items in FIFO order with one actor request. If block=true,
  // waits only until at least one item is available; it never waits to fill the
  // requested batch.
  Task<std::vector<T>> pop_many(size_t max_items, bool block = true,
                                std::optional<td::Timestamp> timeout = std::nullopt) {
    CHECK(max_items > 0);
    while (true) {
      if (!items_.empty()) {
        auto count = std::min(max_items, items_.size());
        std::vector<T> result;
        result.reserve(count);
        for (size_t i = 0; i < count; ++i) {
          result.push_back(std::move(items_.front()));
          items_.pop();
        }
        wake_push_waiters(count);
        co_return std::move(result);
      }
      if (closed_ || !block) {
        co_return td::Status::Error("BackpressureQueue is empty");
      }
      auto [task, promise] = StartedTask<td::Unit>::make_bridge();
      auto waiter_id = ++next_pop_waiter_id_;
      pop_waiters_.push_back(PopWaiter{waiter_id, std::move(promise)});
      td::Result<td::Unit> result;
      if (timeout) {
        result = co_await await_with_timeout(std::move(task), *timeout).wrap();
      } else {
        result = co_await std::move(task).wrap();
      }
      if (result.is_error() && result.error().code() == AWAIT_TIMEOUT_CODE) {
        std::erase_if(pop_waiters_, [&](const PopWaiter& waiter) { return waiter.id == waiter_id; });
        co_return result.move_as_error();
      }
      if (result.is_error() && items_.empty()) {
        co_return td::Status::Error("BackpressureQueue is closed");
      }
    }
  }

  void close() {
    if (closed_) {
      return;
    }
    closed_ = true;
    for (auto& w : push_waiters_) {
      w.set_error(td::Status::Error("BackpressureQueue is closed"));
    }
    push_waiters_.clear();
    for (auto& w : pop_waiters_) {
      w.promise.set_error(td::Status::Error("BackpressureQueue is closed"));
    }
    pop_waiters_.clear();
  }

 private:
  std::queue<T> items_;
  size_t capacity_;
  bool closed_ = false;

  using Waiter = typename StartedTask<td::Unit>::ExternalPromise;
  struct PopWaiter {
    td::uint64 id;
    Waiter promise;
  };
  std::deque<PopWaiter> pop_waiters_;
  std::deque<Waiter> push_waiters_;
  td::uint64 next_pop_waiter_id_{0};

  void wake_pop_waiters(size_t count) {
    while (count-- != 0 && !pop_waiters_.empty()) {
      auto w = std::move(pop_waiters_.front().promise);
      pop_waiters_.pop_front();
      w.set_value(td::Unit());
    }
  }

  void wake_push_waiters(size_t count) {
    while (count-- != 0 && !push_waiters_.empty()) {
      auto w = std::move(push_waiters_.front());
      push_waiters_.pop_front();
      w.set_value(td::Unit());
    }
  }
};

// Copyable handle to a BackpressureQueue actor. Safe to pass into lambdas.
template <typename T>
class BackpressureQueue {
  using A = BackpressureQueueActor<T>;

 public:
  BackpressureQueue() = default;

  explicit BackpressureQueue(td::Slice name, size_t capacity) {
    actor_ = std::make_shared<ActorOwn<A>>(create_actor<A>(name, capacity));
  }

  StartedTask<bool> push(T item) {
    CHECK(actor_);
    return ask(*actor_, &A::push, std::move(item), true);
  }

  StartedTask<bool> try_push(T item) {
    CHECK(actor_);
    return ask(*actor_, &A::push, std::move(item), false);
  }

  StartedTask<size_t> push_many(std::vector<T> items) {
    CHECK(actor_);
    return ask(*actor_, &A::push_many, std::move(items), std::numeric_limits<size_t>::max(), true);
  }

  StartedTask<size_t> try_push_many(std::vector<T> items) {
    CHECK(actor_);
    return ask(*actor_, &A::push_many, std::move(items), std::numeric_limits<size_t>::max(), false);
  }

  StartedTask<size_t> push_many_bounded(std::vector<T> items, size_t occupancy_limit) {
    CHECK(actor_);
    return ask(*actor_, &A::push_many, std::move(items), occupancy_limit, true);
  }

  StartedTask<T> pop() {
    CHECK(actor_);
    return ask(*actor_, &A::pop, true);
  }

  StartedTask<T> try_pop() {
    CHECK(actor_);
    return ask(*actor_, &A::pop, false);
  }

  StartedTask<std::vector<T>> pop_many(size_t max_items) {
    CHECK(actor_);
    return ask(*actor_, &A::pop_many, max_items, true, std::optional<td::Timestamp>{});
  }

  StartedTask<std::vector<T>> try_pop_many(size_t max_items) {
    CHECK(actor_);
    return ask(*actor_, &A::pop_many, max_items, false, std::optional<td::Timestamp>{});
  }

  StartedTask<std::vector<T>> pop_many_until(size_t max_items, td::Timestamp timeout) {
    CHECK(actor_);
    return ask(*actor_, &A::pop_many, max_items, true, std::optional<td::Timestamp>{timeout});
  }

  void close() {
    if (actor_) {
      send_closure(*actor_, &A::close);
    }
  }

 private:
  std::shared_ptr<ActorOwn<A>> actor_;
};

}  // namespace td::actor
