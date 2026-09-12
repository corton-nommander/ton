#include <memory>
#include <set>
#include <vector>

#include "td/actor/TestScheduler.h"
#include "td/utils/tests.h"
#include "validator/impl/ext-message-pool.hpp"

namespace ton::validator {
namespace {

Bits256 producer_bits(td::uint64 value, unsigned prefix = 0) {
  auto result = Bits256::zero();
  result.as_array()[0] = static_cast<unsigned char>(prefix);
  for (unsigned i = 0; i < 8; ++i) {
    result.as_array()[31 - i] = static_cast<unsigned char>(value >> (i * 8));
  }
  return result;
}

// The admission boundary already proved these immutable messages. These
// fixtures exercise the real producer actors and bounded transport, without
// constructing unrelated account/config/cryptography fixtures a second time.
class ProducerMessage final : public ExtMessage {
 public:
  ProducerMessage(StdSmcAddress source, td::uint64 nonce) : source_(source) {
    hash_ = producer_bits(nonce);
    for (unsigned i = 0; i < 8; ++i) { hash_.as_array()[8 + i] = source.as_array()[24 + i]; }
  }
  AccountIdPrefixFull shard() const override { return extract_addr_prefix(basechainId, source_); }
  td::BufferSlice serialize() const override { return {}; }
  td::Ref<vm::Cell> root_cell() const override { return {}; }
  Hash hash() const override { return hash_; }
  Hash hash_norm() const override { return hash_; }
  WorkchainId wc() const override { return basechainId; }
  StdSmcAddress addr() const override { return source_; }

 private:
  StdSmcAddress source_;
  Hash hash_;
};

td::uint64 message_nonce(const ExtMsgQueueEntry &entry) {
  CHECK(entry.message);
  const auto hash = entry.message->first->hash();
  td::uint64 nonce = 0;
  for (unsigned i = 24; i < 32; ++i) { nonce = (nonce << 8) | hash.as_array()[i]; }
  return nonce;
}

}  // namespace

class NativeProducerActorTestAccess {
 public:
  using Address = ExtMessagePool::NativeAddress;
  using Fence = ExtMessagePool::NativeProducerFence;
  using Inbox = ExtMessagePool::NativeProducerInbox;
  using View = ExtMessagePool::NativePublishedSource;
  using ViewPtr = ExtMessagePool::NativePublishedSourcePtr;
  using Telemetry = ExtMessagePool::NativeProducerTelemetry;

  static ShardIdFull lane() { return shard_prefix(extract_addr_prefix(basechainId, producer_bits(1)), 1); }

  class Producer final : public ExtMessagePool {
   public:
    Producer(std::shared_ptr<Inbox> inbox, std::shared_ptr<Telemetry> telemetry,
             std::shared_ptr<ExtMsgQueueTelemetry> transport)
        : ExtMessagePool(lane(), std::move(inbox), std::move(telemetry), std::move(transport), 16384) {
    }
    void consume() { NativeProducerActorTestAccess::consume(*this); }
    void install(std::unique_ptr<ExtMsgCallback> callback) {
      NativeProducerActorTestAccess::install(*this, std::move(callback));
    }
    void wake(Address source) { NativeProducerActorTestAccess::wake(*this, source); }
    std::size_t blocked_pumps() { return NativeProducerActorTestAccess::blocked_pumps(*this); }
  };

  class Owner final : public ExtMessagePool {
   public:
    Owner() : ExtMessagePool({}, {}) {}
    void start_up() override {}
    std::pair<std::size_t, td::uint64> install_with_queued_publication(
        std::vector<std::unique_ptr<ExtMsgCallback>> callbacks) {
      return NativeProducerActorTestAccess::install_with_queued_publication(*this, std::move(callbacks));
    }
    td::uint64 publications() {
      return NativeProducerActorTestAccess::publications(*this);
    }
  };

  static std::shared_ptr<View> view(unsigned source_id, std::shared_ptr<Fence> fence,
                                    std::size_t count, td::uint32 logical_count = 1) {
    auto result = std::make_shared<View>();
    result->source = {basechainId, producer_bits(source_id)};
    result->fence = std::move(fence);
    result->invalidation = result->fence->invalidation.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
      const auto nonce = i * logical_count;
      result->messages.push_back({td::make_ref<ProducerMessage>(result->source.second, nonce), nonce,
                                  logical_count, 0, td::Timestamp::in(60), {}, true});
    }
    if (count != 0) {
      result->maximum_priority = result->minimum_priority = 0;
    }
    return result;
  }

  static void queue(std::shared_ptr<Inbox> inbox, ViewPtr view) {
    std::lock_guard lock(inbox->mutex);
    inbox->latest[view->source] = std::move(view);
    inbox->scheduled = true;
    inbox->enqueued_at = td::Time::now();
  }

 private:
  static std::pair<std::size_t, td::uint64> install_with_queued_publication(
      ExtMessagePool &owner, std::vector<std::unique_ptr<ExtMsgCallback>> callbacks) {
    owner.native_persistent_producer_enabled_ = true;
    owner.native_payment_lane_depth_ = 1;
    // The first callback creates an empty producer. Its generic snapshot must
    // survive while native sources are announced later in this same owner turn.
    auto generic = td::make_ref<ProducerMessage>(producer_bits(9000), 0);
    auto mempool = std::make_shared<ExtMessagePool::MempoolMsg>(generic);
    mempool->in_mempool = true;
    ExtMessagePool::MessageId generic_id{generic->shard(), generic->hash()};
    owner.ext_msgs_[3].generic_messages_ = owner.ext_msgs_[3].generic_messages_.insert(generic_id, mempool);
    CHECK(!callbacks.empty());
    owner.install_collator_queue(lane(), std::move(callbacks.front()));
    // Each reservation is already verified and committed, as in the other
    // producer fixtures. Use the real owner publication and install methods;
    // no source view or producer inbox is injected in this regression.
    for (unsigned i = 1; i <= 600; ++i) {
      Address source{basechainId, producer_bits(i)};
      auto message = td::make_ref<ProducerMessage>(source.second, 0);
      auto mempool = std::make_shared<ExtMessagePool::MempoolMsg>(message);
      mempool->native_nonce = 0;
      mempool->native_nonce_count = 1;
      mempool->in_mempool = true;
      ExtMessagePool::MessageId id{message->shard(), message->hash()};
      auto &tier = owner.ext_msgs_[0];
      tier.ext_messages_ = tier.ext_messages_.insert(id, mempool);
      tier.native_messages_ = tier.native_messages_.insert({0, id.dst, id.hash}, mempool);
      tier.ext_addr_messages_[source].emplace(id.hash, id);
      owner.ext_messages_hashes_[id.hash] = {0, id};
      owner.ext_messages_hashes_norm_[id.hash].insert({0, id});
      auto &watermark = owner.native_nonce_watermarks_[source];
      CHECK(watermark.observe_account_state(0, 100, 1));
      auto &work = owner.native_accounts_[source].messages[0];
      work.hash = id.hash;
      work.source = source;
      work.logical_count = 1;
      work.amount = 1;
      work.valid_until = 10000;
      work.account_revision = watermark.revision;
      work.committed = true;
      work.set_mempool_link(mempool, 0, id);
      owner.mark_native_producer_source(source);
    }
    for (auto &callback : callbacks) {
      if (callback) { owner.install_collator_queue(lane(), std::move(callback)); }
    }
    CHECK(owner.native_producer_flush_scheduled_);
    const auto &endpoint = owner.native_producers_.at(lane());
    return {endpoint.inbox->owner_pending_sources.load(), owner.native_producer_telemetry_->publications.load()};
  }
  static td::uint64 publications(const ExtMessagePool &owner) {
    return owner.native_producer_telemetry_->publications.load();
  }
  static void consume(ExtMessagePool &pool) { pool.consume_native_producer_updates(); }
  static void install(ExtMessagePool &pool, std::unique_ptr<ExtMsgCallback> callback) {
    auto installed = std::make_shared<ExtMessagePool::InstalledCallback>(std::move(callback));
    installed->callback->queue_state->attach_telemetry(pool.native_transport_telemetry_);
    pool.begin_callback_epoch(installed);
    pool.install_native_producer_callback(std::move(installed));
  }
  static void wake(ExtMessagePool &pool, Address source) {
    std::set<Address> changed{source};
    pool.wake_native_callbacks(&changed);
  }
  static std::size_t blocked_pumps(const ExtMessagePool &pool) {
    std::size_t result = 0;
    for (const auto &callback : pool.callbacks_) { result += callback->pump_active; }
    return result;
  }
};

namespace {

using Access = NativeProducerActorTestAccess;
using Producer = Access::Producer;

struct Runtime {
  std::shared_ptr<Access::Inbox> inbox{std::make_shared<Access::Inbox>()};
  std::shared_ptr<Access::Telemetry> telemetry{std::make_shared<Access::Telemetry>()};
  std::shared_ptr<ExtMsgQueueTelemetry> transport{std::make_shared<ExtMsgQueueTelemetry>()};
  td::actor::ActorOwn<Producer> producer{
      td::actor::create_actor<Producer>("native-producer-fixture", inbox, telemetry, transport)};
};

struct Consumer {
  ExtMsgQueue queue;
  std::shared_ptr<ExtMsgQueueState> state{std::make_shared<ExtMsgQueueState>()};
  std::size_t transport_capacity;
  explicit Consumer(std::size_t capacity = 1)
      : queue("native-producer-fixture-transport", capacity + 1), transport_capacity(capacity) {
  }
  std::unique_ptr<ExtMsgCallback> callback(std::size_t logical_limit,
                                           NativeSourceNonceFloors floors = {}) const {
    auto result = std::make_unique<ExtMsgCallback>();
    result->shard = Access::lane();
    result->queue = queue;
    result->queue_state = state;
    result->queue_capacity = logical_limit;
    result->transport_message_capacity = transport_capacity;
    result->native_streaming = true;
    result->timeout = td::Timestamp::in(30);
    result->native_source_nonce_floors = std::move(floors);
    return result;
  }
};

td::actor::Task<std::vector<ExtMsgQueueEntry>> collect_epoch(Consumer &consumer) {
  std::vector<ExtMsgQueueEntry> result;
  for (std::size_t i = 0; i < 16385; ++i) {
    auto entry = co_await consumer.queue.pop();
    if (entry.is_completion()) {
      consumer.state->observe_completion(entry.completed_epoch);
      co_return result;
    }
    CHECK(entry.native);
    consumer.state->record_consumed(1, entry.logical_native_count());
    result.push_back(std::move(entry));
  }
  co_return td::Status::Error("producer failed to complete its bounded callback");
}

}  // namespace

TEST(NativeMessageProducerActor, RepeatedInstallsReuseQueuedPublicationAndKeepInitialGenericSnapshot) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto owner = td::actor::create_actor<Access::Owner>("native-producer-owner-fixture");
    Consumer first(8), second(8), third(8);
    std::vector<std::unique_ptr<ExtMsgCallback>> callbacks;
    for (auto *consumer : {&first, &second, &third}) { callbacks.push_back(consumer->callback(1024)); }
    auto immediate = co_await td::actor::ask(owner, &Access::Owner::install_with_queued_publication,
                                            std::move(callbacks));
    // The empty producer creation and two subsequent installs occurred in one
    // owner turn. The latter two must not consume the queued publication token
    // synchronously or replace it with another one.
    ASSERT_EQ(immediate.first, 600u);
    ASSERT_EQ(immediate.second, 0u);
    for (auto *consumer : {&first, &second, &third}) {
      std::set<StdSmcAddress> native_sources;
      std::size_t generic_count = 0;
      for (std::size_t count = 0; count <= 601; ++count) {
        auto entry = co_await consumer->queue.pop();
        if (entry.is_completion()) {
          consumer->state->observe_completion(entry.completed_epoch);
          break;
        }
        CHECK(entry.message);
        if (entry.native) {
          ASSERT_EQ(message_nonce(entry), 0u);
          ASSERT_TRUE(native_sources.insert(entry.message->first->addr()).second);
          consumer->state->record_consumed(1, entry.logical_native_count());
        } else {
          ++generic_count;
          ASSERT_TRUE(entry.message->first->addr() == producer_bits(9000));
          ASSERT_EQ(entry.message->second, 3);
        }
      }
      ASSERT_EQ(native_sources.size(), 600u);
      ASSERT_EQ(generic_count, 1u);
      ASSERT_TRUE(!consumer->state->producer_pending());
    }
    ASSERT_EQ(co_await td::actor::ask(owner, &Access::Owner::publications), 3u);
    co_return {};
  });
}

TEST(NativeMessageProducerActor, SiblingCallbacksKeepExactFloorsAndIntactRunsIndependent) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    Runtime runtime;
    auto fence = std::make_shared<Access::Fence>();
    fence->canonical_next_nonce.store(16, std::memory_order_release);
    auto view = Access::view(1, fence, 6, 16);
    Access::queue(runtime.inbox, view);
    co_await td::actor::ask(runtime.producer, &Producer::consume);
    Consumer first(2), sibling(1);
    co_await td::actor::ask(runtime.producer, &Producer::install,
        first.callback(64, {{basechainId, view->source.second, 32}}));
    co_await td::actor::ask(runtime.producer, &Producer::install,
        sibling.callback(32, {{basechainId, view->source.second, 64}}));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(co_await td::actor::ask(runtime.producer, &Producer::blocked_pumps), 2u);
    auto first_entries = co_await collect_epoch(first);
    auto sibling_entries = co_await collect_epoch(sibling);
    ASSERT_EQ(first_entries.size(), 4u);
    ASSERT_EQ(sibling_entries.size(), 2u);
    for (std::size_t i = 0; i < first_entries.size(); ++i) {
      ASSERT_EQ(message_nonce(first_entries[i]), 32 + i * 16);
      ASSERT_EQ(first_entries[i].logical_native_count(), 16u);
    }
    for (std::size_t i = 0; i < sibling_entries.size(); ++i) {
      ASSERT_EQ(message_nonce(sibling_entries[i]), 64 + i * 16);
      ASSERT_EQ(sibling_entries[i].logical_native_count(), 16u);
    }
    ASSERT_EQ(fence->canonical_next_nonce.load(), 16u);
    ASSERT_EQ(fence->invalidation.load(), 0u);
    ASSERT_TRUE(!first.state->producer_pending() && !sibling.state->producer_pending());
    co_return {};
  });
}

TEST(NativeMessageProducerActor, CanonicalFenceAdvancesBeforeReplacementDelivery) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    Runtime runtime;
    auto fence = std::make_shared<Access::Fence>();
    auto view = Access::view(2, fence, 8);
    // A blocked old prefix must not hold a newer canonical head hostage while
    // the owner replacement is delayed in another actor's mailbox.
    for (std::size_t i = 0; i < 4; ++i) {
      view->messages[i].active = false;
      view->messages[i].reactivate_at = td::Timestamp::in(20);
    }
    Access::queue(runtime.inbox, view);
    co_await td::actor::ask(runtime.producer, &Producer::consume);
    Consumer consumer;
    co_await td::actor::ask(runtime.producer, &Producer::install, consumer.callback(16));
    ASSERT_TRUE((co_await collect_epoch(consumer)).empty());
    fence->canonical_next_nonce.store(4, std::memory_order_release);
    co_await td::actor::ask(runtime.producer, &Producer::wake, view->source);
    auto entries = co_await collect_epoch(consumer);
    ASSERT_EQ(entries.size(), 4u);
    for (std::size_t i = 0; i < entries.size(); ++i) { ASSERT_EQ(message_nonce(entries[i]), 4 + i); }
    ASSERT_EQ(runtime.telemetry->index_updates.load(), 1u);
    co_return {};
  });
}

TEST(NativeMessageProducerActor, AnnouncedPublicationKeepsEpochOpenUntilFinalSourceArrives) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    Runtime runtime;
    runtime.inbox->owner_pending_sources.store(2, std::memory_order_release);
    Consumer consumer;
    co_await td::actor::ask(runtime.producer, &Producer::install, consumer.callback(16));
    co_await scheduler.wait_sync_work();
    ASSERT_TRUE((co_await consumer.queue.try_pop().wrap()).is_error());
    ASSERT_TRUE(consumer.state->producer_pending());

    Access::queue(runtime.inbox, Access::view(10, std::make_shared<Access::Fence>(), 1));
    runtime.inbox->owner_pending_sources.store(1, std::memory_order_release);
    co_await td::actor::ask(runtime.producer, &Producer::consume);
    auto first = co_await consumer.queue.pop();
    ASSERT_TRUE(first.message && first.native);
    ASSERT_TRUE(first.message->first->addr() == producer_bits(10));
    consumer.state->record_consumed(1, first.logical_native_count());
    co_await scheduler.wait_sync_work();
    ASSERT_TRUE((co_await consumer.queue.try_pop().wrap()).is_error());
    ASSERT_TRUE(consumer.state->producer_pending());

    Access::queue(runtime.inbox, Access::view(11, std::make_shared<Access::Fence>(), 1));
    runtime.inbox->owner_pending_sources.store(0, std::memory_order_release);
    co_await td::actor::ask(runtime.producer, &Producer::consume);
    auto remaining = co_await collect_epoch(consumer);
    ASSERT_EQ(remaining.size(), 1u);
    ASSERT_TRUE(remaining[0].message->first->addr() == producer_bits(11));
    ASSERT_TRUE(!consumer.state->producer_pending());
    ASSERT_EQ(runtime.transport->consumed.load(), 2u);
    co_return {};
  });
}

TEST(NativeMessageProducerActor, RemovedUnseenSourceIsSkippedAfterBlockedPumpResumes) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    Runtime runtime;
    std::shared_ptr<Access::Fence> removed_fence;
    // Source 1 retains a ready suffix while the lazy membership cursor passes
    // hundreds of single-parent sources. Source 700 remains unseen when the
    // first 512-message pump fragment blocks on its one-message transport.
    for (unsigned i = 1; i <= 900; ++i) {
      auto fence = std::make_shared<Access::Fence>();
      if (i == 700) { removed_fence = fence; }
      Access::queue(runtime.inbox, Access::view(i, fence, i == 1 ? 600 : 1));
    }
    co_await td::actor::ask(runtime.producer, &Producer::consume);
    // Drain the producer's bounded publication turns before capturing the
    // callback membership snapshot used by this stale-cursor regression.
    co_await scheduler.wait_sync_work();
    Consumer consumer;
    co_await td::actor::ask(runtime.producer, &Producer::install, consumer.callback(4096));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(co_await td::actor::ask(runtime.producer, &Producer::blocked_pumps), 1u);
    ASSERT_TRUE(runtime.transport->push_reserved.load() != 0);
    ASSERT_TRUE(consumer.state->native_selected_ahead() <= 1025u);
    removed_fence->invalidation.fetch_add(1, std::memory_order_release);
    Access::queue(runtime.inbox, Access::view(700, removed_fence, 0));
    co_await td::actor::ask(runtime.producer, &Producer::consume);
    auto entries = co_await collect_epoch(consumer);
    ASSERT_EQ(entries.size(), 1498u);
    std::set<std::pair<StdSmcAddress, td::uint64>> delivered;
    for (const auto &entry : entries) {
      const auto source = entry.message->first->addr();
      ASSERT_TRUE(source != producer_bits(700));
      ASSERT_TRUE(delivered.emplace(source, message_nonce(entry)).second);
    }
    ASSERT_TRUE(!consumer.state->producer_pending());
    co_return {};
  });
}

TEST(NativeMessageProducerActor, InvalidatedProjectionWaitsForReplacementThenReactivates) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    Runtime runtime;
    auto fence = std::make_shared<Access::Fence>();
    auto stale = Access::view(3, fence, 4);
    Access::queue(runtime.inbox, stale);
    co_await td::actor::ask(runtime.producer, &Producer::consume);
    fence->invalidation.fetch_add(1, std::memory_order_release);
    Consumer consumer;
    co_await td::actor::ask(runtime.producer, &Producer::install, consumer.callback(16));
    ASSERT_TRUE((co_await collect_epoch(consumer)).empty());
    ASSERT_TRUE(runtime.telemetry->invalidated_probes.load() != 0);
    auto postponed = Access::view(3, fence, 4);
    postponed->messages[0].active = false;
    postponed->messages[0].reactivate_at = td::Timestamp::in(2);
    Access::queue(runtime.inbox, postponed);
    co_await td::actor::ask(runtime.producer, &Producer::consume);
    ASSERT_TRUE((co_await collect_epoch(consumer)).empty());
    scheduler.advance_time(3);
    co_await scheduler.wait_sync_work();
    auto entries = co_await collect_epoch(consumer);
    ASSERT_EQ(entries.size(), 4u);
    for (std::size_t i = 0; i < entries.size(); ++i) { ASSERT_EQ(message_nonce(entries[i]), i); }
    co_return {};
  });
}

TEST(NativeMessageProducerActor, RetiringProducerClosesFullQueueAndAccountsDiscardedWork) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    Runtime runtime;
    Access::queue(runtime.inbox, Access::view(4, std::make_shared<Access::Fence>(), 2048));
    co_await td::actor::ask(runtime.producer, &Producer::consume);
    Consumer consumer;
    co_await td::actor::ask(runtime.producer, &Producer::install, consumer.callback(4096));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(co_await td::actor::ask(runtime.producer, &Producer::blocked_pumps), 1u);
    ASSERT_TRUE(runtime.transport->push_reserved.load() != 0);
    ASSERT_TRUE(consumer.state->native_selected_ahead() <= 1025u);
    runtime.producer.reset();
    co_await scheduler.wait_sync_work();
    std::size_t retained = 0;
    for (unsigned i = 0; i < 3; ++i) {
      auto entry = co_await consumer.queue.try_pop().wrap();
      if (entry.is_error()) {
        break;
      }
      ++retained;
    }
    ASSERT_TRUE(retained <= 1u);
    // The queue's API uses the same "empty" status for nonblocking empty and
    // already-closed empty. A blocking pop must now fail without another wake;
    // an accidentally open queue deadlocks this deterministic scheduler.
    auto closed = co_await consumer.queue.pop().wrap();
    ASSERT_TRUE(closed.is_error());
    ASSERT_EQ(runtime.transport->queued_discarded.load() + runtime.transport->unpushed_discarded.load(),
              runtime.transport->selected.load());
    co_return {};
  });
}

}  // namespace ton::validator
