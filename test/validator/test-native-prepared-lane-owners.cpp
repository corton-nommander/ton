#include <array>
#include <cstdlib>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <thread>

#include "crypto/Ed25519.h"
#include "block/block-parse.h"
#include "vm/boc.h"
#include "vm/dict.h"
#include "td/actor/TestScheduler.h"
#include "td/utils/tests.h"
#include "validator/impl/ext-message-pool.hpp"
#include "validator/impl/shard.hpp"
#include "validator/impl/applied-ext-message-cleanup.hpp"
#include "validator/manager.hpp"

namespace ton::validator {
namespace {

Bits256 owner_bits(unsigned value) {
  auto result = Bits256::zero();
  result.as_array()[28] = static_cast<unsigned char>(value >> 24);
  result.as_array()[29] = static_cast<unsigned char>(value >> 16);
  result.as_array()[30] = static_cast<unsigned char>(value >> 8);
  result.as_array()[31] = static_cast<unsigned char>(value);
  return result;
}

ShardId owner_leaf(unsigned lane) {
  CHECK(lane < 4);
  return (static_cast<ShardId>(lane) * 2 + 1) << 61;
}

BlockIdExt owner_block(WorkchainId wc, ShardId shard, BlockSeqno seqno) {
  auto hash = owner_bits(seqno);
  hash.as_array()[0] = static_cast<unsigned char>(shard >> 56);
  return {BlockId{wc, shard, seqno}, hash, owner_bits(255)};
}

// Keys are chosen by their real public-key prefix, never by overwriting an
// address after signing. Every parent below traverses actual signature checks.
struct OwnerKey {
  td::Ed25519::PrivateKey key;
  StdSmcAddress source;
  static OwnerKey for_lane(unsigned lane) {
    for (;;) {
      auto key = td::Ed25519::generate_private_key().move_as_ok();
      StdSmcAddress source;
      source.as_slice().copy_from(key.get_public_key().move_as_ok().as_octet_string());
      if ((source.as_array()[0] >> 6) == lane) {
        return {std::move(key), source};
      }
    }
  }
  td::Ref<vm::Cell> root(td::uint64 nonce = 0, unsigned count = 16, bool invalid = false,
                        td::uint64 amount = 1, Bits256 domain = owner_bits(1)) const {
    block::NativeTransferRun run;
    run.src = source;
    run.first_nonce = nonce;
    run.valid_until = std::numeric_limits<UnixTime>::max();
    run.outputs.resize(count, {.dst = source, .amount = amount, .fee = 0});
    run.signature = key.sign(run.signing_payload(domain)).move_as_ok().as_slice().str();
    if (invalid) { run.signature[0] ^= 1; }
    vm::CellBuilder builder;
    CHECK(run.store_external(builder));
    return builder.finalize();
  }
};

class OwnerMasterchain final : public MasterchainStateQ {
 public:
  explicit OwnerMasterchain(BlockSeqno seqno = 10, unsigned depth = 2, BlockSeqno shard_seqno = 10)
      : MasterchainStateQ(owner_block(masterchainId, shardIdAll, seqno),
                          vm::CellBuilder{}.store_long(seqno, 32).store_long(depth, 8).finalize_novm()) {
    CHECK(depth <= 2);
    for (unsigned i = 0; i < (1u << depth); ++i) {
      const auto shard = depth == 0 ? shardIdAll : (static_cast<ShardId>(i) * 2 + 1) << (63 - depth);
      shards_.push_back(td::make_ref<block::McShardHash>(owner_block(basechainId, shard, shard_seqno), 0,
                                                       shard_seqno * 100));
    }
  }
  td::Ref<McShardHash> get_shard_from_config(ShardIdFull shard, bool exact) const override {
    for (const auto &candidate : shards_) {
      const auto candidate_shard = candidate->top_block_id().shard_full();
      if (candidate_shard == shard || (!exact && shard_contains(candidate_shard, shard))) {
        return candidate;
      }
    }
    return {};
  }
  std::vector<td::Ref<McShardHash>> get_shards() const override { return shards_; }
  block::SizeLimitsConfig::ExtMsgLimits get_ext_msg_limits() const override { return limits; }
  UnixTime get_unix_time() const override { return 100 + get_seqno(); }
  block::SizeLimitsConfig::ExtMsgLimits limits;
 private:
  std::vector<td::Ref<McShardHash>> shards_;
};

struct OwnerAccount {
  StdSmcAddress source;
  td::uint64 balance{10000};
  td::uint64 nonce{0};
};

class OwnerShard final : public ShardStateQ {
 public:
  OwnerShard(ShardId shard, const std::vector<OwnerAccount> &accounts, BlockSeqno seqno = 10)
      : ShardStateQ(owner_block(basechainId, shard, seqno), make_root(shard, accounts, seqno)) {}
  RootHash root_hash() const override { return RootHash{root_cell()->get_hash().bits()}; }
 private:
  static td::Ref<vm::Cell> make_root(ShardId shard, const std::vector<OwnerAccount> &sources, BlockSeqno seqno) {
    vm::AugmentedDictionary accounts{256, block::tlb::aug_ShardAccounts};
    for (const auto &source : sources) {
      if (!shard_contains(ShardIdFull{basechainId, shard}, extract_addr_prefix(basechainId, source.source))) {
        continue;
      }
      auto account = vm::CellBuilder{}.store_long(1, 2).store_long(source.balance, 64)
          .store_long(source.nonce, 64).store_long(0, 8).finalize_novm();
      auto value = vm::CellBuilder{}.store_ref(account).store_zeroes(256).store_long(0, 64).finalize_novm();
      CHECK(accounts.set(source.source, vm::load_cell_slice_ref(value)));
    }
    auto empty = vm::CellBuilder{}.finalize_novm();
    auto auxiliary = vm::CellBuilder{}.store_zeroes(140).finalize_novm();
    return vm::CellBuilder{}.store_long(0x9023afe2, 32).store_long(0, 32)
        .store_long(shard_pfx_len(shard), 8).store_long(basechainId, 32)
        .store_long(shard & (shard - 1), 64).store_long(seqno, 32).store_long(0, 32)
        .store_long(100 + seqno, 32).store_long(seqno * 100, 64).store_long(0, 32).store_ref(empty)
        .store_long(0, 1).store_ref(accounts.get_wrapped_dict_root()).store_ref(auxiliary)
        .store_long(0, 1).finalize_novm();
  }
};

// A held exact-state fetch allows real owner coroutines to suspend while state
// publication, competing source submissions and deadlines are exercised.
class OwnerManager final : public ValidatorManagerImpl {
 public:
  explicit OwnerManager(std::vector<OwnerAccount> accounts)
      : ValidatorManagerImpl({}, "", {}, {}, {}, {}, {}) {
    for (unsigned lane = 0; lane < 4; ++lane) {
      auto state = td::make_ref<OwnerShard>(owner_leaf(lane), accounts);
      states_.emplace(state->get_block_id(), std::move(state));
    }
  }
  void start_up() override {}
  void wait_block_state_short(BlockIdExt block, td::uint32, td::Timestamp, bool,
                              td::Promise<td::Ref<ShardState>> promise) override {
    if (held_) {
      pending_.emplace_back(block, std::move(promise));
    } else {
      finish(block, std::move(promise));
    }
  }
  void get_block_state_for_litequery(BlockIdExt block, td::Promise<td::Ref<ShardState>> promise) override {
    finish(block, std::move(promise));
  }
  void hold(bool enabled) { held_ = enabled; }
  std::size_t pending() { return pending_.size(); }
  void release() {
    held_ = false;
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto &item : pending) { finish(item.first, std::move(item.second)); }
  }
  void add_states(std::vector<OwnerAccount> accounts, BlockSeqno seqno) {
    for (unsigned lane = 0; lane < 4; ++lane) {
      auto state = td::make_ref<OwnerShard>(owner_leaf(lane), accounts, seqno);
      states_.emplace(state->get_block_id(), std::move(state));
    }
  }
  void get_last_liteserver_state_block(
      td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) override {
    generic_pending_.push_back(std::move(promise));
  }
  std::size_t generic_pending() { return generic_pending_.size(); }
  void fail_generic_state_requests() {
    auto pending = std::move(generic_pending_);
    generic_pending_.clear();
    for (auto &promise : pending) {
      promise.set_error(td::Status::Error(ErrorCode::timeout, "held generic fixture state request timed out"));
    }
  }
 private:
  void finish(const BlockIdExt &block, td::Promise<td::Ref<ShardState>> promise) {
    auto it = states_.find(block);
    if (it == states_.end()) {
      promise.set_error(td::Status::Error("fixture has no requested exact shard state"));
    } else {
      promise.set_value(td::Ref<ShardState>{it->second});
    }
  }
  bool held_{false};
  std::map<BlockIdExt, td::Ref<ShardState>> states_;
  std::vector<std::pair<BlockIdExt, td::Promise<td::Ref<ShardState>>>> pending_;
  std::vector<td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>>> generic_pending_;
};

td::Ref<vm::Cell> owner_generic_root(StdSmcAddress address, unsigned marker,
                                   WorkchainId workchain = basechainId) {
  auto body = vm::CellBuilder{}.store_long(marker, 32).finalize_novm();
  return vm::CellBuilder{}.store_long(2, 2) // ext_in_msg_info$10
      .store_long(0, 2)                    // source addr_none$00
      .store_long(2, 2).store_long(0, 1)   // destination addr_std$10, no anycast
      .store_long(workchain, 8).store_bits(address.bits(), 256)
      .store_long(0, 4)                    // import_fee = 0
      .store_long(0, 1).store_long(1, 1)   // no StateInit, body by reference
      .store_ref(body).finalize_novm();
}

// Generic lifecycle tests begin after validation and use the real finalizer,
// priority indexes and shared permit ownership. Admission tests above use BOCs.
class OwnerGenericMessage final : public ExtMessage {
 public:
  OwnerGenericMessage(StdSmcAddress address, unsigned marker, WorkchainId workchain = basechainId)
      : address_(address), hash_(address), workchain_(workchain) {
    hash_.as_array()[31] ^= static_cast<unsigned char>(marker);
    hash_.as_array()[29] ^= static_cast<unsigned char>(workchain);
  }
  AccountIdPrefixFull shard() const override { return extract_addr_prefix(workchain_, address_); }
  td::BufferSlice serialize() const override { return {}; }
  td::Ref<vm::Cell> root_cell() const override { return vm::CellBuilder{}.store_long(0, 32).finalize_novm(); }
  Hash hash() const override { return hash_; }
  Hash hash_norm() const override { return hash_; }
  WorkchainId wc() const override { return workchain_; }
  StdSmcAddress addr() const override { return address_; }
 private:
  StdSmcAddress address_;
  Hash hash_;
  WorkchainId workchain_;
};

}  // namespace

class NativePreparedLaneOwnerTestAccess {
 public:
  struct View {
    std::shared_ptr<NativePoolOwnerCoordination> coordination;
    std::vector<td::actor::ActorId<ExtMessagePool>> children;
    int index{-1};
    unsigned local_workers{0}, shared_workers{0};
    std::size_t accounts{0}, watermarks{0}, reservations{0}, messages{0};
    td::uint64 generation{0};
    bool ready{false}, failed{false};
    td::uint64 inline_preparations{0};
  };
  static View view(ExtMessagePool &pool) {
    View result{.coordination = pool.native_lane_owner_.coordination, .index = pool.native_lane_owner_.index,
        .local_workers = unsigned(pool.native_signature_verifiers_.size()),
        .shared_workers = unsigned(pool.native_lane_owner_.workers.size()),
        .accounts = pool.native_accounts_.size(), .watermarks = pool.native_nonce_watermarks_.size(),
        .messages = pool.ext_messages_hashes_.size(), .generation = pool.native_lane_applied_generation_,
        .ready = pool.check_native_lane_fence().is_ok(), .failed = pool.native_lane_topology_failed_,
        .inline_preparations = pool.native_lane_inline_preparations_};
    for (const auto &[_, account] : pool.native_accounts_) { result.reservations += account.messages.size(); }
    for (auto &child : pool.native_lane_owners_) { result.children.push_back(child.get()); }
    return result;
  }
  static void publish(ExtMessagePool &pool, td::Ref<MasterchainState> state, unsigned depth = 2,
                       Bits256 domain = owner_bits(1)) {
    auto policy = block::NativePaymentLanePolicy::from_fixed_split_depth(depth, depth).move_as_ok();
    const auto id = state->get_block_id();
    const RootHash root{state->root_cell()->get_hash().bits()};
    pool.native_config_cache_.store(id, root, ExtMessagePool::NativeAdmissionSnapshot{
        .state = state, .block_id = id, .state_root = root, .chain_domain = domain,
        .runs_enabled = true, .payment_lane_policy = std::move(policy)});
    pool.update_last_masterchain_state(std::move(state));
  }
  static void publish_limits(ExtMessagePool &pool, td::uint32 maximum) {
    auto state = td::make_ref<OwnerMasterchain>(11);
    state.write().limits.max_size = maximum;
    publish(pool, std::move(state));
  }
  static void saturate(ExtMessagePool &pool, bool value) {
    pool.native_preparation_parents_ = value ? ExtMessagePool::MAX_NATIVE_PREPARATION_PARENTS : 0;
  }
  static bool rejects_stale_idempotence(ExtMessagePool &pool, td::BufferSlice body) {
    auto message = ExtMessageQ::create_ext_message(std::move(body), pool.last_masterchain_state_->get_ext_msg_limits()).move_as_ok();
    auto result = pool.check_existing_external_message(std::move(message), 0, true);
    return result.is_error() && result.error().code() == ErrorCode::notready;
  }
  static void publish_generation_only(ExtMessagePool &pool) {
    auto &coordination = *pool.native_lane_owner_.coordination;
    std::unique_lock fence(coordination.state_fence);
    coordination.published_generation.fetch_add(1, std::memory_order_release);
  }
  static td::Result<ExtMessage::Hash> insert_generic(ExtMessagePool &pool, StdSmcAddress address) {
    auto message = td::make_ref<OwnerGenericMessage>(address, 7);
    auto [wait, allow] = td::actor::StartedTask<>::make_bridge();
    allow.set_value(td::Unit{});
    auto result = pool.finalize_checked_message(
        ExtMessagePool::CheckResult{.message = message, .wait_allow_broadcast = std::move(wait)},
        0, true, td::Timestamp::in(10));
    if (result.is_error()) { return result.move_as_error(); }
    return message->hash();
  }
  class Worker;
  struct Request {
    td::actor::ActorId<Worker> worker;
    td::BufferSlice body;
    block::SizeLimitsConfig::ExtMsgLimits limits;
    Bits256 domain;
    td::Timestamp deadline;
    bool profile;
    std::shared_ptr<ExtMessagePool::PreparationLease> lease;
    td::Promise<ExtMessagePool::PreparationResult> promise;
  };
  struct Gate { std::vector<Request> pending; };
  class Worker final : public ExtMessagePool::NativeSignatureVerifier {
   public:
    explicit Worker(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
    void prepare(td::BufferSlice body, block::SizeLimitsConfig::ExtMsgLimits limits, Bits256 domain,
        td::Timestamp deadline, bool profile, std::shared_ptr<ExtMessagePool::PreparationLease> lease,
        td::Promise<ExtMessagePool::PreparationResult> promise) override {
      gate_->pending.push_back({actor_id(this), std::move(body), limits, domain, deadline, profile,
                               std::move(lease), std::move(promise)});
    }
    void release(std::size_t index) {
      auto request = std::move(gate_->pending.at(index));
      ExtMessagePool::NativeSignatureVerifier::prepare(std::move(request.body), request.limits, request.domain,
          request.deadline, request.profile, std::move(request.lease), std::move(request.promise));
    }
   private:
    std::shared_ptr<Gate> gate_;
  };
  static void gate(ExtMessagePool &pool, std::shared_ptr<Gate> gate) {
    CHECK(pool.native_lane_owners_.empty());
    pool.native_signature_verifiers_.clear();
    for (unsigned index = 0; index < 8; ++index) {
      pool.native_signature_verifiers_.push_back(td::actor::create_actor<Worker>("owner-preparation-gate", gate));
    }
  }
};

namespace {
using Access = NativePreparedLaneOwnerTestAccess;
class OwnerEnvironment {
 public:
  explicit OwnerEnvironment(bool owners = true) {
    for (const auto *name : {"TON_NATIVE_ADMISSION_SNAPSHOT_REFRESH", "TON_NATIVE_PERSISTENT_PRODUCER",
                            "TON_NATIVE_CANONICAL_JOURNAL", "TON_NATIVE_ADMISSION_SHARD_SHARING"}) { set(name, "0"); }
    set("TON_NATIVE_ADMISSION_CONFIG_CACHE", "1");
    set("TON_NATIVE_ADMISSION_PREPARE", "1");
    set("TON_NATIVE_ADMISSION_PREPARATION_PROFILE", "1");
    set("TON_NATIVE_EXECUTOR_THREADS", "8");
    set("TON_NATIVE_ADMISSION_LANE_OWNERS", owners ? "4" : "0");
  }
  ~OwnerEnvironment() {
    for (const auto &[name, value] : saved_) {
      CHECK((value ? setenv(name.c_str(), value->c_str(), 1) : unsetenv(name.c_str())) == 0);
    }
  }
 private:
  std::map<std::string, std::optional<std::string>> saved_;
  void set(const char *name, const char *value) {
    const auto *old = std::getenv(name);
    saved_[name] = old ? std::optional<std::string>{old} : std::nullopt;
    CHECK(setenv(name, value, 1) == 0);
  }
};

template <class F>
auto on_pool(td::actor::ActorId<ExtMessagePool> pool, F function)
    -> td::actor::Task<decltype(function(std::declval<ExtMessagePool &>()))> {
  using Result = decltype(function(std::declval<ExtMessagePool &>()));
  auto [task, promise] = td::actor::StartedTask<Result>::make_bridge();
  td::actor::send_lambda(pool, [pool, function = std::move(function), promise = std::move(promise)]() mutable {
    promise.set_value(function(pool.get_actor_unsafe()));
  });
  co_return co_await std::move(task);
}

td::actor::Task<> publish(td::actor::ActorId<ExtMessagePool> pool, BlockSeqno seqno = 10,
                          unsigned depth = 2, Bits256 domain = owner_bits(1)) {
  co_await on_pool(pool, [=](ExtMessagePool &actor) {
    Access::publish(actor, td::make_ref<OwnerMasterchain>(seqno, depth), depth, domain);
    return td::Unit{};
  });
  co_return {};
}

td::BufferSlice body(const OwnerKey &key, td::uint64 nonce = 0, int mode = 0) {
  return vm::std_boc_serialize(key.root(nonce), mode).move_as_ok();
}

td::actor::ActorOwn<ExtMessagePool> make_pool(td::actor::ActorId<ValidatorManager> manager, std::size_t limit = 10000) {
  auto opts = ValidatorManagerOptions::create({}, {});
  opts.write().set_max_mempool_num(limit);
  return td::actor::create_actor<ExtMessagePool>("prepared-owner-coordinator", opts, manager);
}

td::actor::Task<Access::View> view(td::actor::ActorId<ExtMessagePool> pool) {
  co_return co_await on_pool(pool, [](ExtMessagePool &actor) { return Access::view(actor); });
}

}  // namespace

TEST(NativePreparedLaneOwners, StartupCreatesFourChildrenAndKeepsCoordinatorLedgerEmptyIncludingLaneZero) {
  OwnerEnvironment env;
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    std::vector<OwnerKey> keys;
    std::vector<OwnerAccount> accounts;
    for (unsigned lane = 0; lane < 4; ++lane) { keys.push_back(OwnerKey::for_lane(lane)); accounts.push_back({keys.back().source}); }
    auto manager = td::actor::create_actor<OwnerManager>("manager", accounts);
    auto pool = make_pool(manager.get());
    const auto bootstrap = co_await view(pool.get());
    ASSERT_EQ(bootstrap.local_workers, 8u);
    ASSERT_TRUE(bootstrap.children.empty());
    ASSERT_TRUE(!bootstrap.ready);
    co_await publish(pool.get());
    co_await scheduler.wait_sync_work();
    std::vector<td::BufferSlice> batch;
    for (const auto &key : keys) { batch.push_back(body(key)); }
    batch.push_back(body(keys[0], 0, 3)); // same signed parent, different valid wire encoding
    auto result = co_await td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_messages_until,
        std::move(batch), 0, true, td::Timestamp::in(10));
    ASSERT_EQ(result.statuses.size(), 5u);
    for (const auto &status : result.statuses) { ASSERT_TRUE(status.accepted); }
    ASSERT_EQ(result.checked_messages.size(), 4u);
    for (auto &checked : result.checked_messages) { co_await std::move(checked.wait_allow_broadcast); }
    auto root = co_await view(pool.get());
    ASSERT_EQ(root.children.size(), 4u);
    ASSERT_EQ(root.accounts, 0u); ASSERT_EQ(root.watermarks, 0u); ASSERT_EQ(root.messages, 0u);
    ASSERT_EQ(root.coordination->count(), 4u);
    for (unsigned lane = 0; lane < 4; ++lane) {
      const auto owner = co_await view(root.children[lane]);
      ASSERT_EQ(owner.index, int(lane)); ASSERT_EQ(owner.local_workers, 0u); ASSERT_EQ(owner.shared_workers, 8u);
      ASSERT_EQ(owner.accounts, 1u); ASSERT_EQ(owner.watermarks, 1u); ASSERT_EQ(owner.reservations, 1u);
    }
    auto retry = co_await td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_message_until,
        body(keys[0]), 0, true, td::Timestamp::in(10));
    ASSERT_TRUE(!retry.should_broadcast);
    ASSERT_TRUE(retry.message.not_null());
    co_await std::move(retry.wait_allow_broadcast);
    co_return {};
  });
}

TEST(NativePreparedLaneOwners, PreparationSelectsFreshExactContextAndBudgetFallbackStillRoutes) {
  for (bool fallback : {false, true}) {
    OwnerEnvironment env;
    td::actor::TestScheduler scheduler;
    scheduler.run([&]() -> td::actor::Task<> {
      auto key = OwnerKey::for_lane(0);
      auto manager = td::actor::create_actor<OwnerManager>("manager", std::vector<OwnerAccount>{{key.source}});
      co_await td::actor::ask(manager.get(), &OwnerManager::add_states, std::vector<OwnerAccount>{{key.source}}, 11u);
      auto pool = make_pool(manager.get());
      auto gate = std::make_shared<Access::Gate>();
      if (!fallback) { co_await on_pool(pool.get(), [gate](auto &actor) { Access::gate(actor, gate); return td::Unit{}; }); }
      co_await publish(pool.get());
      co_await scheduler.wait_sync_work();
      if (fallback) { co_await on_pool(pool.get(), [](auto &actor) { Access::saturate(actor, true); return td::Unit{}; }); }
      auto pending = td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_message_until,
          body(key), 0, true, td::Timestamp::in(10));
      co_await scheduler.wait_sync_work();
      if (!fallback) {
        ASSERT_EQ(gate->pending.size(), 1u);
        co_await publish(pool.get(), 11);
        co_await scheduler.wait_sync_work();
        co_await td::actor::ask(gate->pending[0].worker, &Access::Worker::release, 0u);
      }
      auto result = co_await std::move(pending);
      ASSERT_TRUE(result.should_broadcast);
      co_await std::move(result.wait_allow_broadcast);
      const auto root = co_await view(pool.get());
      ASSERT_EQ(root.accounts, 0u); ASSERT_EQ(root.watermarks, 0u);
      ASSERT_EQ(root.inline_preparations, fallback ? 1u : 0u);
      auto owner = co_await view(root.children[0]);
      ASSERT_EQ(owner.reservations, 1u);
      ASSERT_EQ(owner.generation, root.generation);
      if (fallback) { co_await on_pool(pool.get(), [](auto &actor) { Access::saturate(actor, false); return td::Unit{}; }); }
      co_return {};
    });
  }
}

TEST(NativePreparedLaneOwners, PublicationFenceRejectsOldOwnerBeforeItsUpdateAndIdempotence) {
  OwnerEnvironment env;
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto key = OwnerKey::for_lane(2);
    auto manager = td::actor::create_actor<OwnerManager>("manager", std::vector<OwnerAccount>{{key.source}});
    auto pool = make_pool(manager.get());
    co_await publish(pool.get());
    co_await scheduler.wait_sync_work();
    auto first = co_await td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_message_until,
        body(key), 0, true, td::Timestamp::in(10));
    co_await std::move(first.wait_allow_broadcast);
    auto root = co_await view(pool.get());
    co_await on_pool(pool.get(), [](auto &actor) { Access::publish_generation_only(actor); return td::Unit{}; });
    ASSERT_TRUE(co_await on_pool(root.children[2], [input = body(key)](auto &actor) mutable {
      return Access::rejects_stale_idempotence(actor, std::move(input));
    }));
    // A direct request to the child cannot bypass the shared publication fence.
    auto retry = co_await td::actor::ask(root.children[2], &ExtMessagePool::check_add_external_message_until,
        body(key), 0, true, td::Timestamp::in(10)).wrap();
    ASSERT_TRUE(retry.is_error());
    ASSERT_EQ(retry.error().code(), ErrorCode::notready);
    ASSERT_TRUE(retry.error().message().str().find("current admission state") != std::string::npos);
    ASSERT_EQ((co_await view(root.children[2])).reservations, 1u);
    co_return {};
  });
}

TEST(NativePreparedLaneOwners, StateAdvanceDuringHeldAccountFetchCannotCommit) {
  OwnerEnvironment env;
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto key = OwnerKey::for_lane(3);
    auto manager = td::actor::create_actor<OwnerManager>("manager", std::vector<OwnerAccount>{{key.source}});
    auto pool = make_pool(manager.get());
    co_await publish(pool.get()); co_await scheduler.wait_sync_work();
    co_await td::actor::ask(manager.get(), &OwnerManager::hold, true);
    auto pending = td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_message_until,
        body(key), 0, true, td::Timestamp::in(10));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(co_await td::actor::ask(manager.get(), &OwnerManager::pending), 1u);
    co_await publish(pool.get(), 11); co_await scheduler.wait_sync_work();
    co_await td::actor::ask(manager.get(), &OwnerManager::release);
    auto result = co_await std::move(pending).wrap();
    ASSERT_TRUE(result.is_error());
    ASSERT_EQ(result.error().code(), ErrorCode::notready);
    auto root = co_await view(pool.get());
    const auto owner = co_await view(root.children[3]);
    ASSERT_EQ(owner.reservations, 0u); ASSERT_EQ(owner.watermarks, 0u);
    co_return {};
  });
}

TEST(NativePreparedLaneOwners, LateGenericSiblingKeepsNativeResultAndBroadcastAfterDeadline) {
  OwnerEnvironment env;
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto key = OwnerKey::for_lane(1);
    auto manager = td::actor::create_actor<OwnerManager>("manager", std::vector<OwnerAccount>{{key.source}});
    auto pool = make_pool(manager.get());
    co_await publish(pool.get()); co_await scheduler.wait_sync_work();
    std::vector<td::BufferSlice> inputs;
    inputs.push_back(vm::std_boc_serialize(owner_generic_root(key.source, 9, masterchainId)).move_as_ok());
    inputs.push_back(body(key));
    auto pending = td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_messages_until,
        std::move(inputs), 0, true, td::Timestamp::in(2));
    co_await scheduler.wait_sync_work();
    auto root = co_await view(pool.get());
    ASSERT_EQ((co_await view(root.children[1])).reservations, 1u);
    ASSERT_EQ(co_await td::actor::ask(manager.get(), &OwnerManager::generic_pending), 1u);
    scheduler.advance_time(3);
    co_await td::actor::ask(manager.get(), &OwnerManager::fail_generic_state_requests);
    auto result = co_await std::move(pending);
    ASSERT_TRUE(!result.statuses[0].accepted); ASSERT_TRUE(result.statuses[1].accepted);
    ASSERT_EQ(result.checked_messages.size(), 1u);
    co_await std::move(result.checked_messages[0].wait_allow_broadcast);
    co_return {};
  });
}

TEST(NativePreparedLaneOwners, UnsupportedTopologyLatchesClosedWithoutDroppingChildrenOrWatermarks) {
  OwnerEnvironment env;
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto key = OwnerKey::for_lane(0);
    auto manager = td::actor::create_actor<OwnerManager>("manager", std::vector<OwnerAccount>{{key.source}});
    auto pool = make_pool(manager.get());
    co_await publish(pool.get(), 9, 1); co_await scheduler.wait_sync_work();
    ASSERT_TRUE((co_await view(pool.get())).children.empty());
    co_await publish(pool.get()); co_await scheduler.wait_sync_work();
    auto first = co_await td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_message_until,
        body(key), 0, true, td::Timestamp::in(10));
    co_await std::move(first.wait_allow_broadcast);
    auto initial = co_await view(pool.get());
    co_await publish(pool.get(), 11, 1); co_await scheduler.wait_sync_work();
    co_await publish(pool.get(), 12, 2); co_await scheduler.wait_sync_work();
    auto closed = co_await view(pool.get());
    ASSERT_TRUE(closed.failed && !closed.ready);
    ASSERT_EQ(closed.children.size(), 4u);
    for (unsigned index = 0; index < 4; ++index) { ASSERT_TRUE(closed.children[index] == initial.children[index]); }
    auto owner = co_await view(closed.children[0]);
    ASSERT_EQ(owner.watermarks, 1u); ASSERT_EQ(owner.reservations, 1u); ASSERT_EQ(owner.messages, 1u);
    ASSERT_TRUE(!owner.ready);
    auto retry = co_await td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_message_until,
        body(key), 0, true, td::Timestamp::in(10)).wrap();
    ASSERT_TRUE(retry.is_error());
    ASSERT_EQ(retry.error().code(), ErrorCode::notready);
    co_return {};
  });
}

TEST(NativePreparedLaneOwners, SharedQuotaIncludesCoordinatorGenericAndAllChildCleanup) {
  OwnerEnvironment env;
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto key = OwnerKey::for_lane(0);
    auto manager = td::actor::create_actor<OwnerManager>("manager", std::vector<OwnerAccount>{{key.source}});
    auto pool = make_pool(manager.get(), 1);
    co_await publish(pool.get()); co_await scheduler.wait_sync_work();
    auto hash = co_await on_pool(pool.get(), [&](auto &actor) { return Access::insert_generic(actor, key.source).move_as_ok(); });
    auto full = co_await td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_message_until,
        body(key), 0, true, td::Timestamp::in(10)).wrap();
    ASSERT_TRUE(full.is_error());
    auto root = co_await view(pool.get());
    ASSERT_EQ(root.messages, 1u); ASSERT_EQ(root.accounts, 0u);
    ASSERT_EQ((co_await view(root.children[0])).reservations, 0u);
    co_await td::actor::ask(pool.get(), &ExtMessagePool::complete_external_messages,
        std::vector<ExtMessage::Hash>{}, std::vector<ExtMessage::Hash>{hash});
    auto accepted = co_await td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_message_until,
        body(key), 0, true, td::Timestamp::in(10));
    auto retained = accepted.message; // A retained BOC must not retain a membership permit.
    co_await std::move(accepted.wait_allow_broadcast);
    ASSERT_EQ(root.coordination->count(), 1u);
    co_await td::actor::ask(pool.get(), &ExtMessagePool::erase_external_messages,
        std::vector<ExtMessage::Hash>{retained->hash_norm()});
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(root.coordination->count(), 0u);
    ASSERT_EQ((co_await view(root.children[0])).reservations, 0u);
    auto [task, promise] = td::actor::StartedTask<std::vector<std::pair<std::string, std::string>>>::make_bridge();
    td::actor::send_closure(pool.get(), &ExtMessagePool::prepare_stats_all, std::move(promise));
    const auto stats = co_await std::move(task);
    unsigned identities = 0;
    for (const auto &[name, value] : stats) {
      identities += name.starts_with("native_pool.owner.") && name.ends_with(".identity");
    }
    ASSERT_EQ(identities, 4u);
    co_return {};
  });
}
TEST(NativePreparedLaneOwners, GenericSingleUsesLimitsSelectedAfterPreparation) {
  OwnerEnvironment env;
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto key = OwnerKey::for_lane(1);
    auto manager = td::actor::create_actor<OwnerManager>("manager", std::vector<OwnerAccount>{{key.source}});
    auto pool = make_pool(manager.get());
    auto gate = std::make_shared<Access::Gate>();
    co_await on_pool(pool.get(), [gate](auto &actor) { Access::gate(actor, gate); return td::Unit{}; });
    co_await publish(pool.get()); co_await scheduler.wait_sync_work();
    auto pending = td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_message_until,
        vm::std_boc_serialize(owner_generic_root(key.source, 9)).move_as_ok(), 0, true, td::Timestamp::in(10));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(gate->pending.size(), 1u);
    co_await on_pool(pool.get(), [](auto &actor) { Access::publish_limits(actor, 1); return td::Unit{}; });
    co_await scheduler.wait_sync_work();
    co_await td::actor::ask(gate->pending[0].worker, &Access::Worker::release, 0u);
    auto result = co_await std::move(pending).wrap();
    ASSERT_TRUE(result.is_error());
    ASSERT_EQ(result.error().message().str(), "external message too large, rejecting");
    ASSERT_EQ(co_await td::actor::ask(manager.get(), &OwnerManager::generic_pending), 0u);
    co_return {};
  });
}

TEST(NativePreparedLaneOwners, SyncOnlyMixedCallbackKeepsNativeFirstCapacityAndSiblingFloorsLocal) {
  OwnerEnvironment env;
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto key = OwnerKey::for_lane(0);
    auto manager = td::actor::create_actor<OwnerManager>("manager", std::vector<OwnerAccount>{{key.source}});
    auto pool = make_pool(manager.get());
    co_await publish(pool.get()); co_await scheduler.wait_sync_work();
    co_await on_pool(pool.get(), [&](auto &actor) { return Access::insert_generic(actor, key.source).move_as_ok(); });
    auto accepted = co_await td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_message_until,
        body(key), 0, true, td::Timestamp::in(10));
    co_await std::move(accepted.wait_allow_broadcast);
    const auto root = co_await view(pool.get());
    for (td::uint64 floor : {0u, 16u}) {
      ExtMsgQueue queue("owner-sync-test", 20);
      auto callback = std::make_unique<ExtMsgCallback>();
      callback->shard = {basechainId, owner_leaf(0)};
      callback->queue = queue;
      callback->queue_capacity = 16;
      callback->sync_only = true;
      callback->timeout = td::Timestamp::in(10);
      callback->native_source_nonce_floors.push_back({basechainId, key.source, floor});
      co_await td::actor::ask(pool.get(), &ExtMessagePool::install_collator_queue,
          ShardIdFull{basechainId, owner_leaf(0)}, std::move(callback));
      co_await scheduler.wait_sync_work();
      auto entry = co_await queue.pop();
      ASSERT_TRUE(entry.message);
      ASSERT_EQ(entry.native, floor == 0);
      auto end = co_await queue.pop().wrap();
      ASSERT_TRUE(end.is_error()); // Capacity16 contains one intact16-run OR one generic.
    }
    auto owner = co_await view(root.children[0]);
    ASSERT_EQ(owner.reservations, 1u); ASSERT_EQ(owner.watermarks, 1u);
    co_return {};
  });
}

TEST(NativePreparedLaneOwners, ConflictingSourceParentsKeepHashOrderAcrossOutOfOrderPreparation) {
  OwnerEnvironment env;
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto key = OwnerKey::for_lane(2);
    auto manager = td::actor::create_actor<OwnerManager>("manager", std::vector<OwnerAccount>{{key.source}});
    auto pool = make_pool(manager.get());
    auto gate = std::make_shared<Access::Gate>();
    co_await on_pool(pool.get(), [gate](auto &actor) { Access::gate(actor, gate); return td::Unit{}; });
    co_await publish(pool.get()); co_await scheduler.wait_sync_work();
    auto first = key.root(0, 16, false, 1), second = key.root(0, 16, false, 2);
    if (first->get_hash() < second->get_hash()) { std::swap(first, second); }
    std::vector<td::BufferSlice> inputs;
    inputs.push_back(vm::std_boc_serialize(first).move_as_ok());
    inputs.push_back(vm::std_boc_serialize(second).move_as_ok());
    auto pending = td::actor::ask(pool.get(), &ExtMessagePool::check_add_external_messages_until,
        std::move(inputs), 0, true, td::Timestamp::in(10));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(gate->pending.size(), 2u);
    co_await td::actor::ask(gate->pending[1].worker, &Access::Worker::release, 1u);
    co_await td::actor::ask(gate->pending[0].worker, &Access::Worker::release, 0u);
    auto result = co_await std::move(pending);
    ASSERT_TRUE(!result.statuses[0].accepted && result.statuses[1].accepted);
    ASSERT_EQ(result.checked_messages.size(), 1u);
    ASSERT_TRUE(result.checked_messages[0].message->hash() == second->get_hash());
    co_await std::move(result.checked_messages[0].wait_allow_broadcast);
    co_return {};
  });
}

}  // namespace ton::validator
