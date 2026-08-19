#include "td/utils/tests.h"
#include "td/actor/TestScheduler.h"
#include "validator/impl/ext-message-pool.hpp"

namespace ton::validator {
namespace {

Bits256 make_bits(td::uint32 value, unsigned prefix = 0) {
  Bits256 result = Bits256::zero();
  result.as_array()[0] = static_cast<unsigned char>(prefix);
  result.as_array()[28] = static_cast<unsigned char>(value >> 24);
  result.as_array()[29] = static_cast<unsigned char>(value >> 16);
  result.as_array()[30] = static_cast<unsigned char>(value >> 8);
  result.as_array()[31] = static_cast<unsigned char>(value);
  return result;
}

class FakeExtMessage final : public ExtMessage {
 public:
  FakeExtMessage(StdSmcAddress source, Hash hash)
      : source_(source), hash_(hash), shard_(extract_addr_prefix(basechainId, source)) {
  }

  AccountIdPrefixFull shard() const override {
    return shard_;
  }
  td::BufferSlice serialize() const override {
    return {};
  }
  td::Ref<vm::Cell> root_cell() const override {
    return {};
  }
  Hash hash() const override {
    return hash_;
  }
  Hash hash_norm() const override {
    return hash_;
  }
  WorkchainId wc() const override {
    return basechainId;
  }
  StdSmcAddress addr() const override {
    return source_;
  }

 private:
  StdSmcAddress source_;
  Hash hash_;
  AccountIdPrefixFull shard_;
};

}  // namespace

class ExtMessagePoolTestAccess {
 public:
  using NativeAddress = ExtMessagePool::NativeAddress;

  struct Selection {
    std::vector<NativeAddress> sources;
    std::vector<td::uint64> nonces;
    td::optional<NativeAddress> cursor;
    td::uint64 scanned{0};
    td::uint64 selected{0};
    td::uint64 inactive{0};
    td::uint64 excluded{0};
    td::uint64 ready_sources{0};
    td::uint64 head_gaps{0};
    td::uint64 runs{0};
    td::uint64 max_run_size{0};
  };
  struct SchedulerStats {
    td::uint64 selected{0};
    td::uint64 builds{0};
    td::uint64 source_scans{0};
    td::uint64 source_refreshes{0};
    td::uint64 source_probes{0};
    td::uint64 runs{0};
  };

  static ExtMessagePool make_pool() {
    return ExtMessagePool({}, {});
  }

  static NativeAddress source(unsigned prefix) {
    return {basechainId, make_bits(0, prefix)};
  }

  static ExtMessage::Hash add(ExtMessagePool &pool, NativeAddress source, td::uint64 nonce, int priority = 0,
                              bool active = true, bool committed = true) {
    auto hash = make_bits(static_cast<td::uint32>(nonce + 1), static_cast<unsigned>(source.second.as_array()[0] + 64));
    auto message = td::make_ref<FakeExtMessage>(source.second, hash);
    auto mempool_message = std::make_shared<ExtMessagePool::MempoolMsg>(message);
    mempool_message->native_nonce = nonce;
    mempool_message->active = active;
    if (!active) {
      mempool_message->reactivate_at = td::Timestamp::in(60.0);
    }
    ExtMessagePool::MessageId id{message->shard(), hash};
    auto &messages = pool.ext_msgs_[priority];
    messages.ext_messages_ = messages.ext_messages_.insert(id, mempool_message);
    messages.native_messages_ =
        messages.native_messages_.insert(ExtMessagePool::NativeMessageId{nonce, id.dst, id.hash}, mempool_message);
    messages.ext_addr_messages_[source].emplace(hash, id);
    pool.ext_messages_hashes_[hash] = {priority, id};
    pool.ext_messages_hashes_norm_[hash].insert(ExtMessagePool::NormalizedMessageId{priority, id});

    auto &reservation = pool.native_accounts_[source].messages[nonce];
    reservation.hash = hash;
    reservation.valid_until = std::numeric_limits<td::uint32>::max();
    reservation.account_revision = pool.native_nonce_watermarks_[source].revision;
    reservation.committed = committed;
    return hash;
  }

  static void set_watermark(ExtMessagePool &pool, NativeAddress source, td::uint64 next_nonce) {
    auto &watermark = pool.native_nonce_watermarks_[source];
    watermark.observed_next_nonce = next_nonce;
    watermark.revision = std::max<td::uint64>(watermark.revision, 1);
  }

  static Selection select(ExtMessagePool &pool, ShardIdFull shard, std::size_t limit,
                          td::optional<NativeAddress> cursor = {}, std::vector<ExtMessage::Hash> excluded = {}) {
    std::sort(excluded.begin(), excluded.end());
    auto selected = pool.select_native_messages(shard, excluded, {}, limit, cursor);
    Selection result;
    result.cursor = selected.cursor;
    for (const auto &item : selected.items) {
      result.sources.push_back(item.source);
      result.nonces.push_back(item.nonce);
    }
    result.scanned = selected.counters.scanned;
    result.selected = selected.counters.selected;
    result.inactive = selected.counters.inactive;
    result.excluded = selected.counters.excluded;
    result.ready_sources = selected.counters.ready_sources;
    result.head_gaps = selected.counters.head_gaps;
    result.runs = selected.counters.runs;
    result.max_run_size = selected.counters.max_run_size;
    return result;
  }

  static std::size_t make_due_and_reactivate(ExtMessagePool &pool, const ExtMessage::Hash &hash) {
    auto hash_it = pool.ext_messages_hashes_.find(hash);
    CHECK(hash_it != pool.ext_messages_hashes_.end());
    auto priority_it = pool.ext_msgs_.find(hash_it->second.first);
    CHECK(priority_it != pool.ext_msgs_.end());
    auto message = priority_it->second.ext_messages_.find(hash_it->second.second);
    CHECK(message);
    message.value()->active = false;
    message.value()->reactivate_at = td::Timestamp::now();
    pool.native_reactivations_.emplace(message.value()->reactivate_at, hash_it->second);
    return pool.reactivate_due_native_messages(td::Timestamp::in(0.01));
  }

  static void install_live_waiting_callback(ExtMessagePool &pool, std::size_t queue_capacity) {
    auto callback = std::make_unique<ExtMsgCallback>();
    callback->shard = {basechainId, shardIdAll};
    callback->queue_capacity = queue_capacity;
    callback->timeout = td::Timestamp::in(60.0);
    callback->native_streaming = true;
    auto installed = std::make_shared<ExtMessagePool::InstalledCallback>(std::move(callback));
    // Model the installed callback's serialized pump already waiting. This
    // keeps the unit test actor-free while ensuring the post-commit wake appends
    // work to the existing callback instead of creating another ingress event.
    installed->pump_active = true;
    pool.callbacks_.push_back(std::move(installed));
  }

  static bool finalize_existing_native(ExtMessagePool &pool, NativeAddress source, td::uint64 nonce,
                                       const ExtMessage::Hash &hash, int priority = 0) {
    auto hash_it = pool.ext_messages_hashes_.find(hash);
    CHECK(hash_it != pool.ext_messages_hashes_.end());
    auto message = pool.ext_msgs_[hash_it->second.first].ext_messages_.find(hash_it->second.second);
    CHECK(message);
    block::NativeTransfer transfer{.src = source.second,
                                   .dst = source.second,
                                   .amount = 1,
                                   .fee = 0,
                                   .nonce = nonce,
                                   .valid_until = std::numeric_limits<td::uint32>::max(),
                                   .signature = {}};
    auto [wait_allow_broadcast, allow_broadcast_promise] = td::actor::StartedTask<>::make_bridge();
    allow_broadcast_promise.set_value(td::Unit{});
    ExtMessagePool::CheckResult result{.message = message.value()->message,
                                       .wait_allow_broadcast = std::move(wait_allow_broadcast),
                                       .should_broadcast = true,
                                       .msg_seqno = {},
                                       .native_transfer = transfer};
    return pool.finalize_checked_message(std::move(result), priority, true, td::Timestamp::never()).is_ok();
  }

  static bool reservation_committed(const ExtMessagePool &pool, NativeAddress source, td::uint64 nonce) {
    return pool.native_accounts_.at(source).messages.at(nonce).committed;
  }

  static bool callback_has_delivery(const ExtMessagePool &pool, const ExtMessage::Hash &hash) {
    return pool.callbacks_.size() == 1 && pool.callbacks_.front()->delivered_native.contains(hash) &&
           pool.callbacks_.front()->pending_native.size() == 1 &&
           pool.callbacks_.front()->pending_native.front().message &&
           pool.callbacks_.front()->pending_native.front().message->first->hash() == hash;
  }

  static bool callback_contains_delivery(const ExtMessagePool &pool, const ExtMessage::Hash &hash) {
    if (pool.callbacks_.size() != 1 || !pool.callbacks_.front()->delivered_native.contains(hash)) {
      return false;
    }
    return std::any_of(pool.callbacks_.front()->pending_native.begin(),
                       pool.callbacks_.front()->pending_native.end(), [&](const auto &entry) {
                         return entry.message && entry.message->first->hash() == hash;
                       });
  }

  static std::size_t fill_once(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    auto callback = pool.callbacks_.front();
    pool.begin_callback_epoch(callback);
    return pool.fill_callback_native(callback, false);
  }

  static std::size_t fill_sources(ExtMessagePool &pool, const std::set<NativeAddress> &sources) {
    CHECK(pool.callbacks_.size() == 1);
    auto callback = pool.callbacks_.front();
    pool.begin_callback_epoch(callback);
    return pool.fill_callback_native(callback, false, &sources);
  }

  static SchedulerStats scheduler_stats(const ExtMessagePool &pool) {
    const auto &stats = pool.native_queue_counters_;
    return SchedulerStats{.selected = stats.selected,
                          .builds = stats.scheduler_builds,
                          .source_scans = stats.source_scans,
                          .source_refreshes = stats.source_refreshes,
                          .source_probes = stats.source_probes,
                          .runs = stats.runs};
  }

  static std::size_t callback_pending(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.callbacks_.front()->pending_native.size() + pool.callbacks_.front()->pending_generic.size();
  }

  static std::size_t wake_sources(ExtMessagePool &pool, const std::set<NativeAddress> &sources,
                                  bool preserve_valid_ready_head = false) {
    return pool.wake_native_callbacks(&sources, preserve_valid_ready_head);
  }

  static std::size_t dirty_sources(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.callbacks_.front()->native_dirty_sources.size();
  }

  static std::size_t resume_native_pump_refill(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    auto callback = pool.callbacks_.front();
    auto dirty_sources = std::move(callback->native_dirty_sources);
    callback->native_dirty_sources.clear();
    if (callback->native_scheduler_rebuild) {
      callback->native_scheduler = {};
      callback->native_scheduler_rebuild = false;
      dirty_sources.clear();
    }
    return pool.fill_callback_native(callback, false, dirty_sources.empty() ? nullptr : &dirty_sources);
  }

  static constexpr std::size_t native_delivery_chunk() {
    return ExtMessagePool::NATIVE_DELIVERY_CHUNK;
  }

  static constexpr std::size_t max_native_queue_limit() {
    return ExtMessagePool::MAX_NATIVE_COLLATOR_QUEUE_LIMIT;
  }
};

static_assert(ExtMessagePoolTestAccess::max_native_queue_limit() == 65'536);
static_assert(ExtMessagePoolTestAccess::max_native_queue_limit() == block::NativeTransferBatch::max_entries);
static_assert(ExtMessagePoolTestAccess::native_delivery_chunk() == 512);

TEST(ExtMessagePoolScheduler, MissingHeadBlocksFutureNonces) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(1);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  ExtMessagePoolTestAccess::add(pool, source, 1);
  ExtMessagePoolTestAccess::add(pool, source, 2);

  auto blocked = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_TRUE(blocked.nonces.empty());
  ASSERT_EQ(blocked.head_gaps, 1u);

  ExtMessagePoolTestAccess::add(pool, source, 0);
  auto ready = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_EQ(ready.nonces, (std::vector<td::uint64>{0, 1, 2}));
  ASSERT_EQ(ready.head_gaps, 0u);
}

TEST(ExtMessagePoolScheduler, InactiveHeadReactivationReleasesRun) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(2);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto head = ExtMessagePoolTestAccess::add(pool, source, 0, 0, false);
  ExtMessagePoolTestAccess::add(pool, source, 1);

  auto inactive = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_TRUE(inactive.nonces.empty());
  ASSERT_EQ(inactive.inactive, 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::make_due_and_reactivate(pool, head), 1u);

  auto ready = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_EQ(ready.nonces, (std::vector<td::uint64>{0, 1}));
}

TEST(ExtMessagePoolScheduler, RotatesBoundedSourceRunsFairly) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source_a = ExtMessagePoolTestAccess::source(1);
  auto source_b = ExtMessagePoolTestAccess::source(2);
  auto source_c = ExtMessagePoolTestAccess::source(3);
  for (auto source : {source_a, source_b, source_c}) {
    ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
    for (td::uint64 nonce = 0; nonce < 32; ++nonce) {
      ExtMessagePoolTestAccess::add(pool, source, nonce);
    }
  }

  auto first = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_EQ(first.sources.size(), 32u);
  ASSERT_TRUE(std::all_of(first.sources.begin(), first.sources.begin() + 16,
                          [&](const auto &source) { return source == source_a; }));
  ASSERT_TRUE(std::all_of(first.sources.begin() + 16, first.sources.end(),
                          [&](const auto &source) { return source == source_b; }));
  ASSERT_EQ(first.max_run_size, 16u);

  auto second = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32, first.cursor);
  ASSERT_TRUE(std::all_of(second.sources.begin(), second.sources.begin() + 16,
                          [&](const auto &source) { return source == source_c; }));
  ASSERT_TRUE(std::all_of(second.sources.begin() + 16, second.sources.end(),
                          [&](const auto &source) { return source == source_a; }));
}

TEST(ExtMessagePoolScheduler, ExclusionsAdvanceOnlySpeculativeView) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(4);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto nonce_zero = ExtMessagePoolTestAccess::add(pool, source, 0);
  ExtMessagePoolTestAccess::add(pool, source, 1);
  ExtMessagePoolTestAccess::add(pool, source, 2);

  auto child = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32, {}, {nonce_zero});
  ASSERT_EQ(child.nonces, (std::vector<td::uint64>{1, 2}));
  ASSERT_EQ(child.excluded, 1u);

  auto losing_fork = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_EQ(losing_fork.nonces, (std::vector<td::uint64>{0, 1, 2}));
}

TEST(ExtMessagePoolScheduler, MasterchainNeverScansNativeSources) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(5);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  for (td::uint64 nonce = 0; nonce < 32; ++nonce) {
    ExtMessagePoolTestAccess::add(pool, source, nonce);
  }

  auto selected = ExtMessagePoolTestAccess::select(pool, {masterchainId, shardIdAll}, 500);
  ASSERT_TRUE(selected.nonces.empty());
  ASSERT_EQ(selected.scanned, 0u);
  ASSERT_EQ(selected.selected, 0u);
}

TEST(ExtMessagePoolScheduler, PostCommitWakesLiveWaiterWithoutNewIngress) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(6);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto head = ExtMessagePoolTestAccess::add(pool, source, 0, 0, true, false);

  auto before_commit = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 1);
  ASSERT_TRUE(before_commit.nonces.empty());
  ASSERT_EQ(before_commit.head_gaps, 1u);
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());

  ASSERT_TRUE(ExtMessagePoolTestAccess::finalize_existing_native(pool, source, 0, head));
  ASSERT_TRUE(ExtMessagePoolTestAccess::reservation_committed(pool, source, 0));
  ASSERT_EQ(ExtMessagePoolTestAccess::dirty_sources(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::resume_native_pump_refill(pool), 1u);
  ASSERT_TRUE(ExtMessagePoolTestAccess::callback_has_delivery(pool, head));
}

TEST(ExtMessagePoolScheduler, CallbackSelectionIsIncrementalAndChunkBounded) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  for (unsigned source_id = 1; source_id <= 40; ++source_id) {
    auto source = ExtMessagePoolTestAccess::source(source_id);
    ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
    for (td::uint64 nonce = 0; nonce < 16; ++nonce) {
      ExtMessagePoolTestAccess::add(pool, source, nonce);
    }
  }
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());

  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_pending(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
}

TEST(ExtMessagePoolScheduler, PersistentCallbackSchedulerScansSourcesOnlyOnceAcrossChunks) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  constexpr unsigned source_count = 64;
  constexpr td::uint64 nonces_per_source = 32;
  for (unsigned source_id = 1; source_id <= source_count; ++source_id) {
    auto source = ExtMessagePoolTestAccess::source(source_id);
    ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
    for (td::uint64 nonce = 0; nonce < nonces_per_source; ++nonce) {
      ExtMessagePoolTestAccess::add(pool, source, nonce);
    }
  }
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());

  constexpr std::size_t total = source_count * nonces_per_source;
  for (std::size_t selected = 0; selected < total;
       selected += ExtMessagePoolTestAccess::native_delivery_chunk()) {
    ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
  }
  auto stats = ExtMessagePoolTestAccess::scheduler_stats(pool);
  ASSERT_EQ(stats.selected, total);
  ASSERT_EQ(stats.builds, 1u);
  ASSERT_EQ(stats.source_scans, source_count);
  // One initial probe per source, one probe per selected nonce, plus one
  // ready-head revalidation per 16-message source run.
  ASSERT_TRUE(stats.source_probes <= source_count + total + stats.runs + source_count);
}

TEST(ExtMessagePoolScheduler, TargetedRefreshAddsSourceWithoutRebuildingScheduler) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source_a = ExtMessagePoolTestAccess::source(70);
  ExtMessagePoolTestAccess::set_watermark(pool, source_a, 0);
  ExtMessagePoolTestAccess::add(pool, source_a, 0);
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), 1u);

  auto before = ExtMessagePoolTestAccess::scheduler_stats(pool);
  auto source_b = ExtMessagePoolTestAccess::source(71);
  ExtMessagePoolTestAccess::set_watermark(pool, source_b, 0);
  auto source_b_hash = ExtMessagePoolTestAccess::add(pool, source_b, 0);
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_sources(pool, {source_b}), 1u);
  ASSERT_TRUE(ExtMessagePoolTestAccess::callback_contains_delivery(pool, source_b_hash));

  auto after = ExtMessagePoolTestAccess::scheduler_stats(pool);
  ASSERT_EQ(after.builds, before.builds);
  ASSERT_EQ(after.source_scans, before.source_scans);
  ASSERT_EQ(after.source_refreshes, before.source_refreshes + 1);
}

TEST(ExtMessagePoolScheduler, ActivePumpCoalescesIngressWithoutGrowingPendingChunk) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto ready_source = ExtMessagePoolTestAccess::source(72);
  ExtMessagePoolTestAccess::set_watermark(pool, ready_source, 0);
  for (td::uint64 nonce = 0; nonce < 1024; ++nonce) {
    ExtMessagePoolTestAccess::add(pool, ready_source, nonce);
  }
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool,
                                                          ExtMessagePoolTestAccess::max_native_queue_limit());
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_pending(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
  auto selected_before = ExtMessagePoolTestAccess::scheduler_stats(pool).selected;

  // The callback models a pump suspended on a full transport window. Commits
  // behind its already-valid head must not replace that ready token, and a new
  // source is marked once rather than eagerly appending 512 messages per wake.
  std::set<ExtMessagePoolTestAccess::NativeAddress> ready_sources{ready_source};
  for (unsigned i = 0; i < 1024; ++i) {
    ASSERT_EQ(ExtMessagePoolTestAccess::wake_sources(pool, ready_sources, true), 0u);
  }
  ASSERT_EQ(ExtMessagePoolTestAccess::dirty_sources(pool), 0u);

  auto new_source = ExtMessagePoolTestAccess::source(73);
  ExtMessagePoolTestAccess::set_watermark(pool, new_source, 0);
  ExtMessagePoolTestAccess::add(pool, new_source, 0);
  std::set<ExtMessagePoolTestAccess::NativeAddress> new_sources{new_source};
  std::size_t woken = 0;
  for (unsigned i = 0; i < 1024; ++i) {
    woken += ExtMessagePoolTestAccess::wake_sources(pool, new_sources, true);
  }
  ASSERT_EQ(woken, 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::dirty_sources(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_pending(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
  ASSERT_EQ(ExtMessagePoolTestAccess::scheduler_stats(pool).selected, selected_before);
}

TEST(ExtMessagePoolScheduler, CompletionEpochCannotFinishNewerProducerWork) {
  ExtMsgQueueState state;
  auto first = state.begin_producer_epoch();
  ASSERT_TRUE(state.producer_pending());
  state.observe_completion(first);
  ASSERT_TRUE(!state.producer_pending());

  auto second = state.begin_producer_epoch();
  ASSERT_TRUE(second > first);
  state.observe_completion(first);
  ASSERT_TRUE(state.producer_pending());
  state.observe_completion(second);
  ASSERT_TRUE(!state.producer_pending());
}

TEST(ExtMessagePoolScheduler, CompletionMarkerIsFifoAndNotCountedAsTransfer) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<td::Unit> {
    ExtMsgQueue queue("native-transport", 3);
    auto state = std::make_shared<ExtMsgQueueState>();
    auto telemetry = std::make_shared<ExtMsgQueueTelemetry>();
    state->attach_telemetry(telemetry);
    auto epoch = state->begin_producer_epoch();

    auto source = ExtMessagePoolTestAccess::source(9);
    auto first = td::make_ref<FakeExtMessage>(source.second, make_bits(1, 109));
    auto second = td::make_ref<FakeExtMessage>(source.second, make_bits(2, 109));
    state->record_selected(2);
    std::vector<ExtMsgQueueEntry> messages;
    messages.push_back(ExtMsgQueueEntry::make_message({first, 0}, true));
    messages.push_back(ExtMsgQueueEntry::make_message({second, 0}, true));
    auto pushed = co_await queue.push_many_bounded(std::move(messages), 2);
    state->record_pushed(pushed);
    ASSERT_EQ(pushed, 2u);
    auto marker_pushed = co_await queue.push(ExtMsgQueueEntry::make_completion(epoch));
    ASSERT_TRUE(marker_pushed);

    auto entries = co_await queue.pop_many(3);
    ASSERT_EQ(entries.size(), 3u);
    ASSERT_TRUE(entries[0].message);
    ASSERT_TRUE(entries[1].message);
    ASSERT_TRUE(entries[2].is_completion());
    state->record_consumed(2);
    state->observe_completion(entries[2].completed_epoch);
    ASSERT_TRUE(!state->producer_pending());
    ASSERT_EQ(telemetry->selected.load(), 2u);
    ASSERT_EQ(telemetry->pushed.load(), 2u);
    ASSERT_EQ(telemetry->consumed.load(), 2u);
    queue.close();
    co_return td::Unit{};
  });
}

TEST(ExtMessagePoolScheduler, BlockedConsumerMayDrainReservedPushBeforeProducerContinuation) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<td::Unit> {
    ExtMsgQueue queue("native-transport-race", 1);
    auto state = std::make_shared<ExtMsgQueueState>();
    auto telemetry = std::make_shared<ExtMsgQueueTelemetry>();
    state->attach_telemetry(telemetry);

    bool consumer_finished = false;
    auto consumer = [](ExtMsgQueue queue, std::shared_ptr<ExtMsgQueueState> state,
                       bool *consumer_finished) -> td::actor::Task<td::Unit> {
      auto entries = co_await queue.pop_many(1);
      ASSERT_EQ(entries.size(), 1u);
      ASSERT_TRUE(entries.front().message);
      state->record_consumed(1);
      *consumer_finished = true;
      co_return td::Unit{};
    }(queue, state, &consumer_finished);
    auto started_consumer = std::move(consumer).start();
    co_await scheduler.wait_sync_work();

    auto source = ExtMessagePoolTestAccess::source(10);
    auto message = td::make_ref<FakeExtMessage>(source.second, make_bits(1, 110));
    std::vector<ExtMsgQueueEntry> batch;
    batch.push_back(ExtMsgQueueEntry::make_message({message, 0}, true));
    state->record_selected(1);
    state->record_push_started(1);
    auto push = queue.push_many_bounded(std::move(batch), 1);

    // Drain all actor work without awaiting the producer result. The queue has
    // woken the blocked pop, but producer-side exact publication has not run.
    co_await scheduler.wait_sync_work();
    ASSERT_TRUE(consumer_finished);
    ASSERT_EQ(telemetry->pushed.load(), 0u);
    ASSERT_EQ(telemetry->push_reserved.load(), 1u);
    ASSERT_EQ(telemetry->consumed.load(), 1u);
    ASSERT_EQ(telemetry->high_water.load(), 1u);

    auto pushed = co_await std::move(push);
    ASSERT_EQ(pushed, 1u);
    state->record_push_completed(1, pushed);
    ASSERT_EQ(telemetry->pushed.load(), 1u);
    ASSERT_EQ(telemetry->push_reserved.load(), 0u);
    ASSERT_EQ(telemetry->consumed.load(), 1u);
    co_await std::move(started_consumer);
    queue.close();
    co_return td::Unit{};
  });
}

TEST(ExtMessagePoolScheduler, CancellationAccountingReclassifiesLatePushAndDrain) {
  auto telemetry = std::make_shared<ExtMsgQueueTelemetry>();
  ExtMsgQueueState state;
  state.attach_telemetry(telemetry);
  state.record_selected(10);
  state.record_pushed(6);
  state.record_consumed(2);
  state.record_cancel_discarded();
  ASSERT_EQ(telemetry->unpushed_discarded.load(), 4u);
  ASSERT_EQ(telemetry->queued_discarded.load(), 4u);

  // Reserve the complete pump batch before it blocks. Cancellation initially
  // classifies the reservation as queue-owned; exact partial completion moves
  // only the failed suffix back to unpushed-discarded.
  state.record_push_started(4);
  ASSERT_EQ(telemetry->push_reserved.load(), 4u);
  ASSERT_EQ(telemetry->unpushed_discarded.load(), 0u);
  ASSERT_EQ(telemetry->queued_discarded.load(), 8u);
  state.record_push_completed(4, 2);
  state.record_consumed(3);
  ASSERT_EQ(telemetry->selected.load(), 10u);
  ASSERT_EQ(telemetry->pushed.load(), 8u);
  ASSERT_EQ(telemetry->push_reserved.load(), 0u);
  ASSERT_EQ(telemetry->consumed.load(), 5u);
  ASSERT_EQ(telemetry->unpushed_discarded.load(), 2u);
  ASSERT_EQ(telemetry->queued_discarded.load(), 3u);
  ASSERT_EQ(telemetry->selected.load(), telemetry->consumed.load() +
                                                    telemetry->unpushed_discarded.load() +
                                                    telemetry->queued_discarded.load());
}

}  // namespace ton::validator
