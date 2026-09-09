#include <limits>
#include <memory>

#include "crypto/Ed25519.h"
#include "td/actor/TestScheduler.h"
#include "td/utils/port/Clocks.h"
#include "td/utils/tests.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "validator/impl/applied-ext-message-cleanup.hpp"
#include "validator/impl/ext-message-pool.hpp"
#include "validator/impl/shard.hpp"
#include "validator/manager.hpp"
#include "vm/boc.h"
#include "vm/dict.h"

namespace ton::validator {
namespace {

Bits256 bits(unsigned char value) {
  auto result = Bits256::zero();
  result.as_array()[31] = value;
  return result;
}

BlockIdExt block_id(WorkchainId wc, BlockSeqno seqno) {
  return {BlockId{wc, shardIdAll, seqno}, bits(static_cast<unsigned char>(seqno)), bits(254)};
}

struct SnapshotOptions {
  Bits256 domain{bits(1)};
  bool runs_enabled{true};
  td::uint32 lane_depth{0};
  block::SizeLimitsConfig::ExtMsgLimits limits;
};

class RefreshMasterchainState final : public MasterchainStateQ {
 public:
  RefreshMasterchainState(BlockSeqno seqno, SnapshotOptions options)
      : MasterchainStateQ(block_id(masterchainId, seqno),
                          vm::CellBuilder{}.store_long(seqno, 32).finalize_novm()),
        shard_(td::make_ref<block::McShardHash>(block_id(basechainId, seqno), 0, seqno * 100)),
        options_(std::move(options)) {
  }
  td::Ref<McShardHash> get_shard_from_config(ShardIdFull, bool) const override {
    return shard_;
  }
  block::SizeLimitsConfig::ExtMsgLimits get_ext_msg_limits() const override {
    return options_.limits;
  }
  UnixTime get_unix_time() const override {
    return 100 + get_seqno();
  }

 private:
  td::Ref<McShardHash> shard_;
  SnapshotOptions options_;
};

class RefreshShardState final : public ShardStateQ {
 public:
  RefreshShardState(BlockSeqno seqno, const StdSmcAddress &source, td::uint64 balance = 100,
                    td::uint64 nonce = 0)
      : ShardStateQ(block_id(basechainId, seqno), make_root(seqno, source, balance, nonce)) {
  }
  RootHash root_hash() const override {
    return RootHash{root_cell()->get_hash().bits()};
  }

 private:
  static td::Ref<vm::Cell> make_root(BlockSeqno seqno, const StdSmcAddress &source, td::uint64 balance,
                                    td::uint64 nonce) {
    auto account = vm::CellBuilder{}.store_long(1, 2).store_long(balance, 64)
        .store_long(nonce, 64).store_long(0, 8).finalize_novm();
    auto shard_account = vm::CellBuilder{}.store_ref(account).store_zeroes(256).store_long(0, 64).finalize_novm();
    vm::AugmentedDictionary accounts{256, block::tlb::aug_ShardAccounts};
    CHECK(accounts.set(source, vm::load_cell_slice_ref(shard_account)));
    auto empty = vm::CellBuilder{}.finalize_novm();
    auto auxiliary = vm::CellBuilder{}.store_zeroes(140).finalize_novm();
    return vm::CellBuilder{}.store_long(0x9023afe2, 32).store_long(0, 32)
        .store_long(0, 8).store_long(basechainId, 32).store_long(0, 64)
        .store_long(seqno, 32).store_long(0, 32).store_long(100 + seqno, 32)
        .store_long(seqno * 100, 64).store_long(0, 32).store_ref(empty)
        .store_long(0, 1).store_ref(accounts.get_wrapped_dict_root()).store_ref(auxiliary)
        .store_long(0, 1).finalize_novm();
  }
};

struct Requests {
  struct Request {
    BlockIdExt block;
    td::Timestamp deadline;
    td::Promise<td::Ref<ShardState>> promise;
  };
  std::vector<Request> pending;
};

class RefreshManager final : public ValidatorManagerImpl {
 public:
  explicit RefreshManager(std::shared_ptr<Requests> requests)
      : ValidatorManagerImpl({}, "", {}, {}, {}, {}, {}), requests_(std::move(requests)) {
  }
  void start_up() override {
  }
  void wait_block_state_short(BlockIdExt block, td::uint32, td::Timestamp deadline, bool,
                              td::Promise<td::Ref<ShardState>> promise) override {
    requests_->pending.push_back({block, deadline, std::move(promise)});
  }

 private:
  std::shared_ptr<Requests> requests_;
};

struct SignedInput {
  StdSmcAddress source;
  td::Ref<vm::Cell> root;
  td::BufferSlice boc(int mode = 0) const {
    return vm::std_boc_serialize(root, mode).move_as_ok();
  }
};

SignedInput signed_run(bool cross_prefix = false, bool invalid_signature = false,
                       UnixTime valid_until = std::numeric_limits<UnixTime>::max()) {
  auto key = td::Ed25519::generate_private_key().move_as_ok();
  block::NativeTransferRun run;
  run.src.as_slice().copy_from(key.get_public_key().move_as_ok().as_octet_string());
  run.first_nonce = 0;
  run.valid_until = valid_until;
  auto destination = run.src;
  if (cross_prefix) {
    destination.as_array()[0] ^= 0x80;
  }
  run.outputs = {{.dst = destination, .amount = 1, .fee = 0},
                 {.dst = destination, .amount = 1, .fee = 0}};
  run.signature = key.sign(run.signing_payload(bits(1))).move_as_ok().as_slice().str();
  if (invalid_signature) {
    run.signature[0] ^= 1;
  }
  vm::CellBuilder builder;
  CHECK(run.store_external(builder));
  return {run.src, builder.finalize()};
}

}  // namespace

// Seed immutable configuration facts to isolate the real batch coroutine from
// unrelated config construction. Actual BOCs, native account dictionaries,
// shard-state headers, Ed25519 verifiers and reservation/commit paths run.
class NativeAdmissionRefreshBatchTestAccess {
 public:
  static void initialize(ExtMessagePool &pool, bool enabled) {
    pool.native_admission_snapshot_refresh_enabled_ = enabled;
    pool.native_signature_verifiers_.push_back(
        td::actor::create_actor<ExtMessagePool::NativeSignatureVerifier>("refresh-test-signature"));
  }
  static void install(ExtMessagePool &pool, BlockSeqno seqno, const SnapshotOptions &options) {
    auto state = td::make_ref<RefreshMasterchainState>(seqno, options);
    const auto id = state->get_block_id();
    const RootHash root{state->root_cell()->get_hash().bits()};
    td::optional<block::NativePaymentLanePolicy> lane;
    if (options.lane_depth) {
      lane = block::NativePaymentLanePolicy::from_fixed_split_depth(options.lane_depth,
                                                                   options.lane_depth).move_as_ok();
    }
    pool.last_masterchain_state_ = state;
    pool.reset_native_admission_cache_generation(id);
    pool.native_config_cache_.store(id, root, ExtMessagePool::NativeAdmissionSnapshot{
        .state = state, .block_id = id, .state_root = root, .chain_domain = options.domain,
        .runs_enabled = options.runs_enabled, .payment_lane_policy = std::move(lane)});
  }
  static std::size_t reservations(const ExtMessagePool &pool, const StdSmcAddress &source) {
    auto it = pool.native_accounts_.find({basechainId, source});
    return it == pool.native_accounts_.end() ? 0 : it->second.messages.size();
  }
  static std::string diagnostics(const ExtMessagePool &pool) {
    return pool.native_batch_telemetry_string(':');
  }
};

namespace {

class RefreshPool final : public ExtMessagePool {
 public:
  RefreshPool(td::actor::ActorId<ValidatorManager> manager, bool enabled)
      : ExtMessagePool(ValidatorManagerOptions::create({}, {}), manager), enabled_(enabled) {
  }
  void start_up() override {
    NativeAdmissionRefreshBatchTestAccess::initialize(*this, enabled_);
    install(10, {});
  }
  void install(BlockSeqno seqno, SnapshotOptions options) {
    NativeAdmissionRefreshBatchTestAccess::install(*this, seqno, options);
  }
  td::actor::Task<BatchCheckResult> submit(std::vector<td::BufferSlice> inputs, td::Timestamp deadline) {
    co_return co_await check_add_external_messages_until(std::move(inputs), 0, true, deadline);
  }
  std::size_t reservations(StdSmcAddress source) {
    return NativeAdmissionRefreshBatchTestAccess::reservations(*this, source);
  }
  std::string diagnostics() {
    return NativeAdmissionRefreshBatchTestAccess::diagnostics(*this);
  }

 private:
  bool enabled_;
};

bool has_counter(const std::string &stats, const std::string &name, unsigned value) {
  return (stats + ' ').find(" " + name + ':' + std::to_string(value) + ' ') != std::string::npos;
}

}  // namespace

TEST(NativeAdmissionRefreshBatch, ChangedSnapshotRefreshesRealBatchAndKeepsDuplicateResults) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    const auto input = signed_run();
    auto requests = std::make_shared<Requests>();
    auto manager = td::actor::create_actor<RefreshManager>("refresh-manager", requests);
    auto pool = td::actor::create_actor<RefreshPool>("refresh-pool", manager.get(), true);
    std::vector<td::BufferSlice> inputs;
    inputs.push_back(input.boc());
    inputs.push_back(input.boc());
    auto deadline = td::Timestamp::in(10);
    auto pending = td::actor::ask(pool.get(), &RefreshPool::submit, std::move(inputs), deadline);
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->pending.size(), 1u);
    co_await td::actor::ask(pool.get(), &RefreshPool::install, 11u, SnapshotOptions{});
    requests->pending[0].promise.set_value(td::make_ref<RefreshShardState>(10, input.source));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->pending.size(), 2u);
    ASSERT_EQ(requests->pending[0].deadline.get(), deadline.get());
    ASSERT_EQ(requests->pending[1].deadline.get(), deadline.get());
    ASSERT_TRUE(requests->pending[1].block == block_id(basechainId, 11));
    requests->pending[1].promise.set_value(td::make_ref<RefreshShardState>(11, input.source));
    auto result = co_await std::move(pending);
    ASSERT_EQ(result.statuses.size(), 2u);
    ASSERT_TRUE(result.statuses[0].accepted && result.statuses[1].accepted);
    ASSERT_EQ(result.checked_messages.size(), 1u);
    ASSERT_TRUE(result.checked_messages[0].native_admission);
    ASSERT_EQ(result.checked_messages[0].native_admission.value().first_nonce, 0u);
    ASSERT_EQ(result.checked_messages[0].native_admission.value().logical_count, 2u);
    ASSERT_EQ(co_await td::actor::ask(pool.get(), &RefreshPool::reservations, input.source), 1u);
    auto stats = co_await td::actor::ask(pool.get(), &RefreshPool::diagnostics);
    ASSERT_TRUE(has_counter(stats, "snapshot_refresh_attempts", 1));
    ASSERT_TRUE(has_counter(stats, "snapshot_refresh_successes", 1));
    ASSERT_TRUE(has_counter(stats, "snapshot_refresh_signature_reuses", 1));
    ASSERT_TRUE(has_counter(stats, "snapshot_refresh_accepted_messages", 1));
    co_return {};
  });
}

TEST(NativeAdmissionRefreshBatch, SecondSnapshotChangeStillRejectsAndNeverReserves) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    const auto input = signed_run();
    auto requests = std::make_shared<Requests>();
    auto manager = td::actor::create_actor<RefreshManager>("refresh-manager", requests);
    auto pool = td::actor::create_actor<RefreshPool>("refresh-pool", manager.get(), true);
    std::vector<td::BufferSlice> inputs;
    inputs.push_back(input.boc());
    auto pending = td::actor::ask(pool.get(), &RefreshPool::submit, std::move(inputs), td::Timestamp::in(10));
    co_await scheduler.wait_sync_work();
    co_await td::actor::ask(pool.get(), &RefreshPool::install, 11u, SnapshotOptions{});
    requests->pending[0].promise.set_value(td::make_ref<RefreshShardState>(10, input.source));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->pending.size(), 2u);
    co_await td::actor::ask(pool.get(), &RefreshPool::install, 12u, SnapshotOptions{});
    requests->pending[1].promise.set_value(td::make_ref<RefreshShardState>(11, input.source));
    auto result = co_await std::move(pending);
    ASSERT_TRUE(!result.statuses[0].accepted);
    ASSERT_EQ(result.statuses[0].error_message, "native transfer run admission snapshot changed; retry");
    ASSERT_TRUE(result.checked_messages.empty());
    ASSERT_EQ(co_await td::actor::ask(pool.get(), &RefreshPool::reservations, input.source), 0u);
    ASSERT_EQ(requests->pending.size(), 2u);
    auto stats = co_await td::actor::ask(pool.get(), &RefreshPool::diagnostics);
    ASSERT_TRUE(has_counter(stats, "snapshot_refresh_exhausted", 1));
    ASSERT_TRUE(has_counter(stats, "snapshot_refresh_successes", 0));
    co_return {};
  });
}

TEST(NativeAdmissionRefreshBatch, OriginalDeadlineExpiresDuringRefreshRead) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    const auto input = signed_run();
    auto requests = std::make_shared<Requests>();
    auto manager = td::actor::create_actor<RefreshManager>("refresh-manager", requests);
    auto pool = td::actor::create_actor<RefreshPool>("refresh-pool", manager.get(), true);
    std::vector<td::BufferSlice> inputs;
    inputs.push_back(input.boc());
    auto deadline = td::Timestamp::in(2);
    auto pending = td::actor::ask(pool.get(), &RefreshPool::submit, std::move(inputs), deadline);
    co_await scheduler.wait_sync_work();
    co_await td::actor::ask(pool.get(), &RefreshPool::install, 11u, SnapshotOptions{});
    requests->pending[0].promise.set_value(td::make_ref<RefreshShardState>(10, input.source));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->pending.size(), 2u);
    ASSERT_EQ(requests->pending[1].deadline.get(), deadline.get());
    scheduler.advance_time(3);
    co_await scheduler.wait_sync_work();
    auto result = co_await std::move(pending);
    ASSERT_TRUE(!result.statuses[0].accepted);
    ASSERT_TRUE(result.checked_messages.empty());
    auto stats = co_await td::actor::ask(pool.get(), &RefreshPool::diagnostics);
    ASSERT_TRUE(has_counter(stats, "snapshot_refresh_deadlines", 1));
    requests->pending[1].promise.set_error(td::Status::Error(ErrorCode::notready, "late state"));
    co_return {};
  });
}

TEST(NativeAdmissionRefreshBatch, SignedExpiryDuringRefreshCannotUseRemainingRpcBudget) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    const auto input = signed_run(false, false, static_cast<UnixTime>(td::Clocks::system()) + 2);
    auto requests = std::make_shared<Requests>();
    auto manager = td::actor::create_actor<RefreshManager>("refresh-manager", requests);
    auto pool = td::actor::create_actor<RefreshPool>("refresh-pool", manager.get(), true);
    std::vector<td::BufferSlice> inputs;
    inputs.push_back(input.boc());
    auto deadline = td::Timestamp::in(10);
    auto pending = td::actor::ask(pool.get(), &RefreshPool::submit, std::move(inputs), deadline);
    co_await scheduler.wait_sync_work();
    co_await td::actor::ask(pool.get(), &RefreshPool::install, 11u, SnapshotOptions{});
    requests->pending[0].promise.set_value(td::make_ref<RefreshShardState>(10, input.source));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->pending.size(), 2u);
    scheduler.advance_time(3);
    ASSERT_TRUE(!deadline.is_in_past());
    requests->pending[1].promise.set_value(td::make_ref<RefreshShardState>(11, input.source));
    auto result = co_await std::move(pending);
    ASSERT_TRUE(!result.statuses[0].accepted);
    ASSERT_EQ(result.statuses[0].error_message, "native transfer valid_until is in the past");
    ASSERT_TRUE(result.checked_messages.empty());
    ASSERT_EQ(co_await td::actor::ask(pool.get(), &RefreshPool::reservations, input.source), 0u);
    auto stats = co_await td::actor::ask(pool.get(), &RefreshPool::diagnostics);
    ASSERT_TRUE(has_counter(stats, "snapshot_refresh_deadlines", 0));
    ASSERT_TRUE(has_counter(stats, "snapshot_refresh_successes", 0));
    co_return {};
  });
}

TEST(NativeAdmissionRefreshBatch, RefreshedStateRechecksBalanceNonceAndSigningDomain) {
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    td::actor::TestScheduler scheduler;
    scheduler.run([&]() -> td::actor::Task<> {
      const auto input = signed_run();
      auto requests = std::make_shared<Requests>();
      auto manager = td::actor::create_actor<RefreshManager>("refresh-manager", requests);
      auto pool = td::actor::create_actor<RefreshPool>("refresh-pool", manager.get(), true);
      std::vector<td::BufferSlice> inputs;
      inputs.push_back(input.boc());
      auto pending = td::actor::ask(pool.get(), &RefreshPool::submit, std::move(inputs), td::Timestamp::in(10));
      co_await scheduler.wait_sync_work();
      SnapshotOptions options;
      if (scenario == 2) {
        options.domain = bits(2);
      }
      co_await td::actor::ask(pool.get(), &RefreshPool::install, 11u, options);
      requests->pending[0].promise.set_value(td::make_ref<RefreshShardState>(10, input.source));
      co_await scheduler.wait_sync_work();
      ASSERT_EQ(requests->pending.size(), 2u);
      requests->pending[1].promise.set_value(td::make_ref<RefreshShardState>(
          11, input.source, scenario == 0 ? 1u : 100u, scenario == 1 ? 2u : 0u));
      auto result = co_await std::move(pending);
      ASSERT_TRUE(!result.statuses[0].accepted);
      ASSERT_TRUE(result.checked_messages.empty());
      if (scenario == 0) {
        ASSERT_TRUE(result.statuses[0].error_message.find("insufficient source balance") != std::string::npos);
      } else if (scenario == 1) {
        ASSERT_TRUE(result.statuses[0].error_message.find("Too old native nonce") != std::string::npos);
      } else {
        ASSERT_EQ(result.statuses[0].error_message, "Wrong signature");
      }
      ASSERT_EQ(co_await td::actor::ask(pool.get(), &RefreshPool::reservations, input.source), 0u);
      auto stats = co_await td::actor::ask(pool.get(), &RefreshPool::diagnostics);
      ASSERT_TRUE(has_counter(stats, "snapshot_refresh_successes", 0));
      ASSERT_TRUE(has_counter(stats, "snapshot_refresh_signature_reuses", 0));
      co_return {};
    });
  }
}

TEST(NativeAdmissionRefreshBatch, RefreshedModeLaneAndLimitsRejectBeforeNewShardFetch) {
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    td::actor::TestScheduler scheduler;
    scheduler.run([&]() -> td::actor::Task<> {
      const auto input = signed_run(true);
      auto requests = std::make_shared<Requests>();
      auto manager = td::actor::create_actor<RefreshManager>("refresh-manager", requests);
      auto pool = td::actor::create_actor<RefreshPool>("refresh-pool", manager.get(), true);
      std::vector<td::BufferSlice> inputs;
      inputs.push_back(input.boc());
      auto pending = td::actor::ask(pool.get(), &RefreshPool::submit, std::move(inputs), td::Timestamp::in(10));
      co_await scheduler.wait_sync_work();
      SnapshotOptions options;
      if (scenario == 0) {
        options.runs_enabled = false;
      } else if (scenario == 1) {
        options.lane_depth = 1;
      } else {
        options.limits.max_size = 1;
      }
      co_await td::actor::ask(pool.get(), &RefreshPool::install, 11u, options);
      requests->pending[0].promise.set_value(td::make_ref<RefreshShardState>(10, input.source));
      auto result = co_await std::move(pending);
      ASSERT_TRUE(!result.statuses[0].accepted);
      ASSERT_TRUE(result.checked_messages.empty());
      ASSERT_EQ(requests->pending.size(), 1u);
      if (scenario == 0) {
        ASSERT_TRUE(result.statuses[0].error_message.find("capNativeTransferRuns") != std::string::npos);
      } else if (scenario == 1) {
        ASSERT_EQ(result.statuses[0].error_message, "native transfer destination is outside its source payment lane");
      } else {
        ASSERT_EQ(result.statuses[0].error_message, "external message too large, rejecting");
      }
      auto stats = co_await td::actor::ask(pool.get(), &RefreshPool::diagnostics);
      ASSERT_TRUE(has_counter(stats, "snapshot_refresh_attempts", 1));
      ASSERT_TRUE(has_counter(stats, "snapshot_refresh_successes", 0));
      co_return {};
    });
  }
}

TEST(NativeAdmissionRefreshBatch, DuplicateBocEncodingMustMeetRefreshedWireSizeLimit) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    const auto input = signed_run();
    auto small = input.boc(0);
    auto large = input.boc(3);  // index and CRC change wire size, not cell hash
    ASSERT_TRUE(large.size() > small.size());
    SnapshotOptions options;
    options.limits.max_size = static_cast<td::uint32>(small.size());
    auto requests = std::make_shared<Requests>();
    auto manager = td::actor::create_actor<RefreshManager>("refresh-manager", requests);
    auto pool = td::actor::create_actor<RefreshPool>("refresh-pool", manager.get(), true);
    std::vector<td::BufferSlice> inputs;
    inputs.push_back(std::move(small));
    inputs.push_back(std::move(large));
    auto pending = td::actor::ask(pool.get(), &RefreshPool::submit, std::move(inputs), td::Timestamp::in(10));
    co_await scheduler.wait_sync_work();
    co_await td::actor::ask(pool.get(), &RefreshPool::install, 11u, options);
    requests->pending[0].promise.set_value(td::make_ref<RefreshShardState>(10, input.source));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->pending.size(), 2u);
    requests->pending[1].promise.set_value(td::make_ref<RefreshShardState>(11, input.source));
    auto result = co_await std::move(pending);
    ASSERT_TRUE(result.statuses[0].accepted);
    ASSERT_TRUE(!result.statuses[1].accepted);
    ASSERT_EQ(result.statuses[1].error_message, "external message too large, rejecting");
    ASSERT_EQ(result.checked_messages.size(), 1u);
    co_return {};
  });
}

TEST(NativeAdmissionRefreshBatch, DisabledRefreshAndFailedSignatureDoNotStartSecondAttempt) {
  for (unsigned scenario = 0; scenario < 2; ++scenario) {
    td::actor::TestScheduler scheduler;
    scheduler.run([&]() -> td::actor::Task<> {
      const auto input = signed_run(false, scenario == 1);
      auto requests = std::make_shared<Requests>();
      auto manager = td::actor::create_actor<RefreshManager>("refresh-manager", requests);
      auto pool = td::actor::create_actor<RefreshPool>("refresh-pool", manager.get(), scenario != 0);
      std::vector<td::BufferSlice> inputs;
      inputs.push_back(input.boc());
      auto pending = td::actor::ask(pool.get(), &RefreshPool::submit, std::move(inputs), td::Timestamp::in(10));
      co_await scheduler.wait_sync_work();
      co_await td::actor::ask(pool.get(), &RefreshPool::install, 11u, SnapshotOptions{});
      requests->pending[0].promise.set_value(td::make_ref<RefreshShardState>(10, input.source));
      auto result = co_await std::move(pending);
      ASSERT_TRUE(!result.statuses[0].accepted);
      ASSERT_TRUE(result.checked_messages.empty());
      ASSERT_EQ(requests->pending.size(), 1u);
      ASSERT_EQ(result.statuses[0].error_message,
                scenario == 0 ? "native transfer run admission snapshot changed; retry" : "Wrong signature");
      auto stats = co_await td::actor::ask(pool.get(), &RefreshPool::diagnostics);
      ASSERT_TRUE(has_counter(stats, "snapshot_refresh_attempts", 0));
      co_return {};
    });
  }
}

}  // namespace ton::validator
