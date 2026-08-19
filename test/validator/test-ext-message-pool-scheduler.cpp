#include "td/utils/tests.h"
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
           pool.callbacks_.front()->pending.size() == 1 &&
           pool.callbacks_.front()->pending.front().first->hash() == hash;
  }

  static constexpr std::size_t max_native_queue_limit() {
    return ExtMessagePool::MAX_NATIVE_COLLATOR_QUEUE_LIMIT;
  }
};

static_assert(ExtMessagePoolTestAccess::max_native_queue_limit() == 65'536);
static_assert(ExtMessagePoolTestAccess::max_native_queue_limit() == block::NativeTransferBatch::max_entries);

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
  ASSERT_TRUE(ExtMessagePoolTestAccess::callback_has_delivery(pool, head));
}

}  // namespace ton::validator
