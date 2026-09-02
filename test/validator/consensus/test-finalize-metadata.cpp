#include <limits>
#include <type_traits>
#include <vector>

#include "block/signature-set.h"
#include "block/transaction.h"
#include "common/checksum.h"
#include "td/actor/BusRuntime.h"
#include "td/actor/coro_utils.h"
#include "td/utils/tests.h"
#include "validator/consensus/bus.h"
#include "validator/consensus/simplex/state-resolver-policy.h"
#include "validator/consensus/utils.h"

namespace {

using namespace ton;
using namespace ton::validator;
using namespace ton::validator::consensus;

static_assert(!std::is_default_constructible_v<FinalizeBlock>);
static_assert(simplex::classify_native_finalization(9, 10, false) ==
              simplex::NativeFinalizationRelation::canonically_decided);

td::Ref<vm::Cell> make_empty_hashmap() {
  vm::CellBuilder builder;
  CHECK(builder.store_long_bool(0, 1));
  return builder.finalize_novm();
}

BlockCandidate make_candidate(td::Ref<vm::Cell> extra) {
  auto placeholder = vm::CellBuilder{}.finalize_novm();
  vm::CellBuilder builder;
  CHECK(builder.store_long_bool(0x11ef55aa, 32) && builder.store_long_bool(-111, 32) &&
        builder.store_ref_bool(placeholder) && builder.store_ref_bool(placeholder) &&
        builder.store_ref_bool(placeholder) && builder.store_ref_bool(extra));
  auto root = builder.finalize_novm();
  auto data = vm::std_boc_serialize(root, 31).move_as_ok();
  auto shard = ShardIdFull{basechainId, shardIdAll};
  return BlockCandidate{Ed25519_PublicKey{td::Bits256::zero()},
                        BlockIdExt(BlockId(shard, 1), root->get_hash().bits(), td::sha256_bits256(data)),
                        td::Bits256::zero(), data.clone(), {}};
}

BlockCandidate make_opaque_candidate() {
  return make_candidate(vm::CellBuilder{}.finalize_novm());
}

block::NativeTransfer make_native_transfer() {
  block::NativeTransfer transfer;
  transfer.src.as_slice().copy_from(std::string(32, '\x11'));
  transfer.dst.as_slice().copy_from(std::string(32, '\x22'));
  transfer.amount = 17;
  transfer.fee = 3;
  transfer.nonce = 41;
  transfer.valid_until = std::numeric_limits<UnixTime>::max();
  transfer.signature.assign(64, '\x5a');
  CHECK(transfer.is_valid());
  return transfer;
}

BlockCandidate make_native_candidate(const block::NativeTransfer& transfer) {
  block::NativeTransferBatch batch;
  batch.entries.push_back({transfer, 0, 0});
  vm::CellBuilder native_builder;
  CHECK(batch.store(native_builder));
  auto native_batch = native_builder.finalize_novm();

  auto empty = make_empty_hashmap();
  vm::CellBuilder extra_builder;
  CHECK(extra_builder.store_long_bool(0x4a33f6fd, 32) && extra_builder.store_ref_bool(empty) &&
        extra_builder.store_ref_bool(empty) && extra_builder.store_ref_bool(empty) &&
        extra_builder.store_bits_bool(td::Bits256::zero()) && extra_builder.store_bits_bool(td::Bits256::zero()) &&
        extra_builder.store_bool_bool(true) && extra_builder.store_ref_bool(native_batch));
  return make_candidate(extra_builder.finalize_novm());
}

TrackedNativeExternalMessage make_handoff_metadata() {
  TrackedNativeExternalMessage metadata;
  metadata.hash.as_slice().copy_from(std::string(32, '\x33'));
  metadata.workchain = basechainId;
  metadata.source.as_slice().copy_from(std::string(32, '\x44'));
  metadata.nonce = 987654321;
  return metadata;
}

void assert_metadata_equal(const std::vector<TrackedNativeExternalMessage>& actual,
                           const std::vector<TrackedNativeExternalMessage>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    ASSERT_TRUE(actual[i].hash == expected[i].hash);
    ASSERT_EQ(actual[i].workchain, expected[i].workchain);
    ASSERT_TRUE(actual[i].source == expected[i].source);
    ASSERT_EQ(actual[i].nonce, expected[i].nonce);
  }
}

class HandoffManager final : public ManagerFacade {
 public:
  struct Snapshot {
    std::size_t accept_calls;
    std::vector<std::vector<TrackedNativeExternalMessage>> tracked_batches;
  };

  td::actor::Task<GeneratedCandidate> collate_block(CollateParams, td::CancellationToken) final {
    co_return td::Status::Error("not used by finalized metadata handoff test");
  }

  td::actor::Task<ValidateCandidateResult> validate_block_candidate(BlockCandidate, ValidateParams,
                                                                     td::Timestamp) final {
    co_return td::Status::Error("not used by finalized metadata handoff test");
  }

  td::actor::Task<> accept_block(BlockIdExt, td::Ref<BlockData>, size_t, td::Ref<block::BlockSignatureSet>, int,
                                 bool) final {
    ++accept_calls_;
    if (cancel_accept_) {
      co_return td::Status::Error(ErrorCode::cancelled, "simulated losing candidate");
    }
    co_return td::Unit{};
  }

  td::actor::Task<> track_external_messages(std::vector<TrackedNativeExternalMessage> messages) final {
    tracked_batches_.push_back(std::move(messages));
    co_return td::Unit{};
  }

  td::actor::Task<td::Ref<vm::Cell>> wait_block_state_root(BlockIdExt, td::Timestamp) final {
    co_return td::Status::Error("not used by finalized metadata handoff test");
  }

  td::actor::Task<td::Ref<BlockData>> wait_block_data(BlockIdExt, td::Timestamp) final {
    co_return td::Status::Error("not used by finalized metadata handoff test");
  }

  td::actor::Task<> set_cancel_accept(bool value) {
    cancel_accept_ = value;
    co_return td::Unit{};
  }

  td::actor::Task<Snapshot> snapshot() {
    co_return Snapshot{accept_calls_, tracked_batches_};
  }

 private:
  bool cancel_accept_ = false;
  std::size_t accept_calls_ = 0;
  std::vector<std::vector<TrackedNativeExternalMessage>> tracked_batches_;
};

struct HandoffBus final : Bus {
  using Parent = Bus;
  using Events = td::TypeList<>;

  void populate_collator_schedule() final {
  }

  ~HandoffBus() final {
    td::actor::SchedulerContext::get().stop();
  }

  td::actor::ActorId<HandoffManager> handoff_manager;
};

CandidateRef make_opaque_finalized_candidate() {
  auto block = make_opaque_candidate();
  return td::make_ref<Candidate>(CandidateId{7, td::Bits256::zero()}, std::nullopt, PeerValidatorId{0},
                                 std::move(block), td::BufferSlice{});
}

td::Ref<block::BlockSignatureSet> make_signature_set() {
  return block::BlockSignatureSet::create_ordinary({}, 0, 0);
}

class HandoffPublisher final : public td::actor::SpawnsWith<HandoffBus>, public td::actor::ConnectsTo<> {
 public:
  void start_up() final {
    run().start().detach();
  }

 private:
  td::actor::Task<> run() {
    auto candidate = make_opaque_finalized_candidate();
    // The event deliberately carries nonempty metadata for a candidate whose
    // body has no native batch. Re-decoding in BlockAccepter would therefore
    // track an empty vector instead of this source/nonce identity.
    ASSERT_TRUE(get_candidate_native_external_messages(std::get<BlockCandidate>(candidate->block)).move_as_ok().empty());
    auto expected = std::vector<TrackedNativeExternalMessage>{make_handoff_metadata()};

    auto accepted_event = std::make_shared<FinalizeBlock>(candidate, make_signature_set(), expected);
    auto accepted_result = co_await owning_bus().publish(accepted_event).wrap();
    ASSERT_TRUE(accepted_result.is_ok());
    ASSERT_TRUE(accepted_event->native_external_messages.empty());

    auto accepted_snapshot = co_await td::actor::ask(owning_bus()->handoff_manager, &HandoffManager::snapshot);
    ASSERT_EQ(accepted_snapshot.accept_calls, 1u);
    ASSERT_EQ(accepted_snapshot.tracked_batches.size(), 1u);
    assert_metadata_equal(accepted_snapshot.tracked_batches.front(), expected);

    co_await td::actor::ask(owning_bus()->handoff_manager, &HandoffManager::set_cancel_accept, true);
    auto cancelled_event = std::make_shared<FinalizeBlock>(candidate, make_signature_set(), expected);
    auto cancelled_result = co_await owning_bus().publish(cancelled_event).wrap();
    ASSERT_TRUE(cancelled_result.is_error());
    ASSERT_EQ(cancelled_result.error().code(), ErrorCode::cancelled);
    // A rejected accept must neither track nor consume event-owned metadata.
    assert_metadata_equal(cancelled_event->native_external_messages, expected);

    auto cancelled_snapshot = co_await td::actor::ask(owning_bus()->handoff_manager, &HandoffManager::snapshot);
    ASSERT_EQ(cancelled_snapshot.accept_calls, 2u);
    ASSERT_EQ(cancelled_snapshot.tracked_batches.size(), 1u);

    owning_bus().publish<StopRequested>();
    stop();
    co_return td::Unit{};
  }
};

TEST(FinalizeMetadataHandoff, ParsesRealCompactNativeBatch) {
  auto transfer = make_native_transfer();
  auto candidate = make_native_candidate(transfer);
  auto messages = get_candidate_native_external_messages(candidate).move_as_ok();
  auto hash = transfer.external_hash().move_as_ok();

  ASSERT_EQ(messages.size(), 1u);
  ASSERT_TRUE(messages[0].hash == hash);
  ASSERT_EQ(messages[0].workchain, basechainId);
  ASSERT_TRUE(messages[0].source == transfer.src);
  ASSERT_EQ(messages[0].nonce, transfer.nonce);
}

TEST(FinalizeMetadataHandoff, MovesValueOwnedMetadataOnlyAfterAccept) {
  td::actor::Scheduler scheduler({1});
  td::actor::Runtime runtime;
  BlockAccepter::register_in(runtime);
  runtime.register_actor<HandoffPublisher>("HandoffPublisher");

  td::actor::ActorOwn<HandoffManager> manager;
  scheduler.run_in_context([&] {
    manager = td::actor::create_actor<HandoffManager>("HandoffManager");
    auto bus = std::make_shared<HandoffBus>();
    bus->shard = ShardIdFull{basechainId, shardIdAll};
    bus->local_id.idx = PeerValidatorId{0};
    bus->manager = manager.get();
    bus->handoff_manager = manager.get();
    runtime.start(std::move(bus));
  });
  scheduler.run();
}

}  // namespace
