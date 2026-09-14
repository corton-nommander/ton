#include "native-parent-metadata-fixtures.h"

#include <map>
#include <type_traits>

#include "crypto/Ed25519.h"
#include "td/utils/tests.h"

namespace {
using block::NativeTransferBatch;
using block::NativeTransferRun;
using block::CanonicalNativeTransferRun;
using namespace native_metadata_test;

static_assert(std::is_same_v<decltype(std::declval<CanonicalNativeTransferRun&>().run()),
                             const NativeTransferRun&>);
static_assert(!std::is_default_constructible_v<CanonicalNativeTransferRun>);

void check_rooted_equivalence(td::Ref<vm::Cell> root, const NativeTransferBatch& full) {
  const auto rooted = NativeTransferBatch::unpack_rooted_runs(root).move_as_ok();
  ASSERT_EQ(rooted.version, full.version);
  ASSERT_EQ(rooted.logical_entry_count(), full.entries.size());
  auto restored = serialize(rooted);
  ASSERT_EQ(restored->get_hash(), root->get_hash());
  const auto restored_boc = vm::std_boc_serialize(restored, 31).move_as_ok();
  const auto original_boc = vm::std_boc_serialize(root, 31).move_as_ok();
  ASSERT_EQ(restored_boc.as_slice(), original_boc.as_slice());
  if (!NativeTransferBatch::is_direct_run_version(full.version)) {
    ASSERT_TRUE(rooted.rooted_runs.empty());
    ASSERT_EQ(rooted.entries.size(), full.entries.size());
    ASSERT_TRUE(rooted.accounts == full.accounts);
    return;
  }
  ASSERT_TRUE(rooted.entries.empty());
  ASSERT_TRUE(rooted.accounts.empty());
  ASSERT_TRUE(rooted.runs.empty());
  ASSERT_EQ(rooted.rooted_runs.size(), full.runs.size());
  std::size_t index = 0;
  for (std::size_t parent = 0; parent < rooted.rooted_runs.size(); ++parent) {
    const auto& sealed = rooted.rooted_runs[parent];
    ASSERT_EQ(sealed.run().signing_payload(), full.runs[parent].signing_payload());
    ASSERT_EQ(sealed.run().signature, full.runs[parent].signature);
    ASSERT_EQ(ton::Bits256{sealed.root()->get_hash().bits()}, full.runs[parent].external_hash().move_as_ok());
    ASSERT_TRUE(sealed.for_each_transfer([&](const block::NativeTransfer& transfer) {
      ASSERT_TRUE(index < full.entries.size());
      ASSERT_EQ(transfer.signing_payload(), full.entries[index].transfer.signing_payload());
      ASSERT_EQ(transfer.signature, full.entries[index].transfer.signature);
      ++index;
      return true;
    }));
  }
  ASSERT_EQ(index, full.entries.size());
}

void check_projection(td::Ref<vm::Cell> root) {
  const auto full = NativeTransferBatch::unpack(root).move_as_ok();
  check_rooted_equivalence(root, full);
  const auto projected = NativeTransferBatch::unpack_external_metadata(root).move_as_ok();
  const bool direct = NativeTransferBatch::is_direct_run_version(full.version);
  ASSERT_EQ(projected.size(), direct ? full.runs.size() : full.entries.size());
  std::size_t logical_count = 0;
  for (std::size_t index = 0; index < projected.size(); ++index) {
    const auto& metadata = projected[index];
    if (direct) {
      const auto& parent = full.runs[index];
      ASSERT_EQ(metadata.hash, parent.external_hash().move_as_ok());
      ASSERT_EQ(metadata.source, parent.src);
      ASSERT_EQ(metadata.nonce, parent.first_nonce);
      ASSERT_EQ(metadata.logical_count, parent.outputs.size());
      // Flattened child entries have a synthetic NTFX identity, which is not
      // the source-authorized NTRN parent identity even for one-output runs.
      ASSERT_TRUE(metadata.hash != full.entries[logical_count].transfer.external_hash().move_as_ok());
    } else {
      const auto& transfer = full.entries[index].transfer;
      ASSERT_EQ(metadata.hash, transfer.external_hash().move_as_ok());
      ASSERT_EQ(metadata.source, transfer.src);
      ASSERT_EQ(metadata.nonce, transfer.nonce);
      ASSERT_EQ(metadata.logical_count, 1u);
    }
    logical_count += metadata.logical_count;
  }
  ASSERT_EQ(logical_count, full.entries.size());
}

void check_rejected(td::Ref<vm::Cell> root) {
  ASSERT_TRUE(NativeTransferBatch::unpack(root).is_error());
  ASSERT_TRUE(NativeTransferBatch::unpack_rooted_runs(root).is_error());
  ASSERT_TRUE(NativeTransferBatch::unpack_external_metadata(root).is_error());
}

TEST(NativeParentMetadata, ScalarVersionsAndEmptyBatchesPreserveFullDecoderSemantics) {
  for (td::uint8 version = 1; version <= 6; ++version) {
    NativeTransferBatch batch;
    batch.version = version;
    check_projection(serialize(batch));
    if (version <= 4) {
      batch.entries = {{scalar(17), 101, 102}, {scalar(18), 103, 104}, {scalar(21, 2), 105, 106}};
      check_projection(serialize(batch));
    }
  }
}

TEST(NativeParentMetadata, DirectVersionsPreserveOrderedParentsAndDuplicates) {
  for (const td::uint8 version : {td::uint8{5}, td::uint8{6}}) {
    NativeTransferBatch batch;
    batch.version = version;
    batch.runs = {native_metadata_test::run(1, 80, 9), native_metadata_test::run(2, 40, 3), native_metadata_test::run(16, 42, 3), native_metadata_test::run(2, 40, 3)};
    auto root = serialize(batch);
    check_projection(root);
    auto projected = NativeTransferBatch::unpack_external_metadata(root).move_as_ok();
    ASSERT_EQ(projected.size(), 4u);
    ASSERT_EQ(projected[1].hash, projected[3].hash);
    ASSERT_EQ(projected[0].nonce, 80u);
    ASSERT_EQ(projected[1].nonce, 40u);
    ASSERT_EQ(projected[2].logical_count, 16u);
  }
}

TEST(NativeParentMetadata, RootedRunPairingBoundariesAndEmptyBatches) {
  for (const td::uint8 version : {td::uint8{5}, td::uint8{6}}) {
    for (const std::size_t count : {0u, 1u, 2u, 3u, 15u, 16u, 17u, 31u, 32u, 33u, 63u, 64u, 65u}) {
      check_projection(serialize(direct_batch(version, count)));
      check_projection(serialize(direct_batch(version, count, 1)));
    }
  }
}

TEST(NativeParentMetadata, RootedRunsOwnCanonicalFieldsAndRejectMixedRepresentations) {
  auto source = native_metadata_test::run(3);
  auto input_root = serialize(source);
  const auto original_hash = input_root->get_hash();
  auto sealed = CanonicalNativeTransferRun::unpack_external(input_root).move_as_ok();
  input_root.clear();
  source.signature[0] ^= 1;
  source.outputs[0].amount += 1;
  source.first_nonce += 1;
  ASSERT_EQ(sealed.root()->get_hash(), original_hash);
  ASSERT_EQ(serialize(sealed.run())->get_hash(), original_hash);

  NativeTransferBatch batch;
  batch.version = 6;
  batch.rooted_runs.push_back(sealed);
  auto copied = batch;
  batch.rooted_runs.clear();
  ASSERT_EQ(NativeTransferBatch::unpack(serialize(copied)).move_as_ok().entries.size(), 3u);
  auto rejects = [](const NativeTransferBatch& value) {
    vm::CellBuilder builder;
    ASSERT_TRUE(!value.store(builder));
  };
  batch = copied;
  batch.runs.push_back(sealed.run());
  rejects(batch);
  batch = copied;
  batch.entries = NativeTransferBatch::flatten_runs({sealed.run()}).move_as_ok();
  rejects(batch);
  batch = copied;
  batch.accounts.push_back(sealed.run().src);
  rejects(batch);
  for (td::uint8 version = 1; version <= 4; ++version) {
    batch = copied;
    batch.version = version;
    rejects(batch);
  }
  batch = copied;
  auto moved = std::move(batch.rooted_runs.front());
  rejects(batch);
  ASSERT_TRUE(!batch.rooted_runs.front().for_each_transfer([](const block::NativeTransfer&) { return true; }));
  ASSERT_EQ(moved.root()->get_hash(), original_hash);
  ASSERT_TRUE(CanonicalNativeTransferRun::unpack_external({}).is_error());

  batch = NativeTransferBatch::unpack_rooted_runs(serialize(direct_batch(6, NativeTransferBatch::max_entries))).move_as_ok();
  batch.rooted_runs.push_back(sealed);
  rejects(batch);
}

TEST(NativeParentMetadata, RootedReplayMatchesExpandedStateAndFirstFailureOrder) {
#if TD_HAVE_OPENSSL
  using Result = block::NativeTransferStateResult;
  auto key = td::Ed25519::generate_private_key().move_as_ok();
  ton::Bits256 domain = address(77);
  auto parent = native_metadata_test::run(4);
  parent.src.as_slice().copy_from(key.get_public_key().move_as_ok().as_octet_string());
  parent.valid_until = 200;
  parent.outputs = {{parent.src, 10, 2}, {address(2), 20, 3}, {address(2), 30, 5}, {address(3), 40, 7}};
  struct Replay {
    td::uint64 nonce;
    std::map<ton::StdSmcAddress, td::uint64> balances;
    std::vector<Result::Code> results;
  };
  for (unsigned scenario = 0; scenario < 6; ++scenario) {
    const bool terminal_nonce = scenario == 5;
    parent.first_nonce = terminal_nonce ? std::numeric_limits<td::uint64>::max() - 3 : 100;
    parent.signature = key.sign(parent.signing_payload(domain)).move_as_ok().as_slice().str();
    auto sealed = CanonicalNativeTransferRun::unpack_external(serialize(parent)).move_as_ok();
    ASSERT_TRUE(sealed.run().verify_signature(domain).is_ok());
    ASSERT_TRUE(sealed.run().verify_signature(address(78)).is_error());
    const auto expanded = NativeTransferBatch::flatten_runs({parent}).move_as_ok();
    const td::uint64 initial_balance = scenario == 1 ? 50 : 200;
    const td::uint64 initial_nonce = parent.first_nonce + ((scenario == 2 || scenario == 3) ? 1 : 0);
    const ton::UnixTime now = scenario == 3 || scenario == 4 ? 200 : 199;
    auto replay = [&](bool use_rooted) {
      Replay state{initial_nonce, {{parent.src, initial_balance}}, {}};
      auto apply = [&](const block::NativeTransfer& transfer) {
        const bool same = transfer.src == transfer.dst;
        block::NativeTransferStateInput input;
        input.transfer = &transfer;
        input.src_balance = state.balances[transfer.src];
        input.src_nonce = state.nonce;
        input.src_status = block::Account::acc_uninit;
        input.src_is_native = true;
        input.dst_balance = state.balances[transfer.dst];
        input.dst_status = scenario == 4 ? block::Account::acc_active : block::Account::acc_uninit;
        input.dst_is_native = true;
        input.same_account = same;
        auto result = block::execute_native_transfer_state(input, now, false);
        state.results.push_back(result.code);
        if (result.code != Result::ok) {
          return false;
        }
        state.nonce = result.src_nonce;
        state.balances[transfer.src] = result.src_balance;
        if (!same) state.balances[transfer.dst] = result.dst_balance;
        return true;
      };
      if (use_rooted) {
        const bool complete = sealed.for_each_transfer(apply);
        ASSERT_EQ(complete, scenario == 0);
      } else {
        for (const auto& entry : expanded) {
          if (!apply(entry.transfer)) break;
        }
      }
      return state;
    };
    const auto old_result = replay(false);
    const auto new_result = replay(true);
    ASSERT_EQ(new_result.nonce, old_result.nonce);
    ASSERT_TRUE(new_result.balances == old_result.balances);
    ASSERT_TRUE(new_result.results == old_result.results);
    const Result::Code expected[] = {Result::ok, Result::insufficient_balance, Result::nonce_mismatch,
                                     Result::expired, Result::invalid_destination, Result::nonce_overflow};
    ASSERT_EQ(new_result.results.back(), expected[scenario]);
    const std::size_t expected_calls[] = {4, 3, 1, 1, 1, 4};
    ASSERT_EQ(new_result.results.size(), expected_calls[scenario]);
    if (scenario == 0) {
      ASSERT_EQ(new_result.balances.at(parent.src), 93u);
      ASSERT_EQ(new_result.balances.at(address(2)), 50u);
      ASSERT_EQ(new_result.nonce, 104u);
    }
  }
#endif
}

TEST(NativeParentMetadata, BocContainerFlagsDoNotChangeParentIdentity) {
  auto batch = direct_batch(6, 48);
  auto root = serialize(batch);
  auto expected = NativeTransferBatch::unpack_external_metadata(root).move_as_ok();
  for (const int mode : {0, 1, 2, 3, 31}) {
    auto boc = vm::std_boc_serialize(root, mode).move_as_ok();
    auto restored = vm::std_boc_deserialize(boc).move_as_ok();
    ASSERT_EQ(restored->get_hash(), root->get_hash());
    check_projection(restored);
    auto projected = NativeTransferBatch::unpack_external_metadata(restored).move_as_ok();
    ASSERT_EQ(projected.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
      ASSERT_EQ(projected[index].hash, expected[index].hash);
    }
  }
}

TEST(NativeParentMetadata, MaximumLogicalAndPhysicalCountsStayWithinProtocol) {
  // Both the largest child materialization and the largest parent vector are
  // legal. They must not be confused when enforcing the logical header limit.
  check_projection(serialize(direct_batch(5, NativeTransferBatch::max_entries, 16)));
  check_projection(serialize(direct_batch(6, NativeTransferBatch::max_entries, 1)));
}

TEST(NativeParentMetadata, TerminalNonceIsDecodedWithoutInventingAnExclusiveCursor) {
  NativeTransferBatch batch;
  batch.version = 6;
  batch.runs = {native_metadata_test::run(16, std::numeric_limits<td::uint64>::max() - 15)};
  check_projection(serialize(batch));
  // The metadata API carries the interval; the consensus floor projection
  // separately rejects an exclusive cursor that cannot fit in uint64.
  auto parent = batch.runs[0];
  parent.first_nonce++;
  vm::CellBuilder canonical_builder;
  ASSERT_TRUE(batch.runs[0].store_external(canonical_builder));
  auto parts = vm::load_cell_slice(canonical_builder.finalize());
  auto signature_root = parts.prefetch_ref(0);
  auto output_root = parts.prefetch_ref(1);
  check_rejected(header(6, 0, 16, {}, raw_run(parent, signature_root, output_root)));
}

TEST(NativeParentMetadata, RejectsMalformedHeadersAndDirectAccountTables) {
  auto parent = serialize(native_metadata_test::run(2));
  check_rejected({});
  for (const td::uint8 version : {td::uint8{5}, td::uint8{6}}) {
    check_rejected(header(version, 0, 2, {}, parent, 0));
    check_rejected(header(7, 0, 2, {}, parent));
    check_rejected(header(version, 0, 2, {}, parent, NativeTransferBatch::magic, true));
    check_rejected(header(version, 0, NativeTransferBatch::max_entries + 1, {}, parent));
    check_rejected(header(version, NativeTransferBatch::max_accounts + 1, 2, parent, parent));
    check_rejected(header(version, 1, 2, parent, parent));
    check_rejected(header(version, 0, 2, parent, parent));
    check_rejected(header(version, 1, 2, {}, parent));
    check_rejected(header(version, 0, 0, {}, parent));
    check_rejected(header(version, 0, 2));
    check_rejected(header(version, 0, 1, {}, parent));
    check_rejected(header(version, 0, 3, {}, parent));
  }
  // Scalar versions retain their original strict checks through the shared
  // full parse: no projection-specific shortcut may skip malformed tables.
  for (td::uint8 version = 1; version <= 4; ++version) {
    check_rejected(header(version, 0, 1, {}, parent));
    check_rejected(header(version, 2, 1, parent, parent));
  }
}

TEST(NativeParentMetadata, RejectsRunBranchesWithWrongWeightsTagsOrTrailingData) {
  const auto first = serialize(native_metadata_test::run(2));
  const auto second = serialize(native_metadata_test::run(1, 102));
  for (const td::uint8 version : {td::uint8{5}, td::uint8{6}}) {
    for (const auto left_count : {0u, 1u, 3u, 4u}) {
      check_rejected(header(version, 0, 3, {}, branch(NativeTransferBatch::runs_node_magic, left_count, first, second)));
    }
    check_rejected(header(version, 0, 3, {}, branch(0, 2, first, second)));
    check_rejected(header(version, 0, 3, {}, branch(NativeTransferBatch::runs_node_magic, 2, first, {})));
    check_rejected(header(version, 0, 3, {}, branch(NativeTransferBatch::runs_node_magic, 2, first, second, 32, true)));
  }
}

TEST(NativeParentMetadata, RejectsNoncanonicalEquivalentRunTreeShapes) {
  auto first = serialize(native_metadata_test::run(2));
  auto second = serialize(native_metadata_test::run(1, 102));
  auto third = serialize(native_metadata_test::run(1, 103));
  // Store canonicalizes three parents to [[first, second], third]. This
  // semantically equivalent alternative has valid counts but a different root.
  auto alternate = branch(NativeTransferBatch::runs_node_magic, 2, first,
                          branch(NativeTransferBatch::runs_node_magic, 1, second, third));
  for (const td::uint8 version : {td::uint8{5}, td::uint8{6}}) {
    check_rejected(header(version, 0, 4, {}, alternate));
  }
}

TEST(NativeParentMetadata, RejectsMalformedSignedParentsAfterValidPrefix) {
  const auto parent = native_metadata_test::run(2);
  const auto outputs = output_leaf(parent, 0, 2);
  const auto valid_signature = signature();
  std::vector<td::Ref<vm::Cell>> invalid_parents{
      raw_run(parent, signature(63), outputs),
      raw_run(parent, signature(65), outputs),
      raw_run(parent, signature(64, true), outputs),
      raw_run(parent, {}, outputs),
      raw_run(parent, valid_signature, {}),
      raw_run(parent, valid_signature, outputs, 0),
      raw_run(parent, valid_signature, outputs, NativeTransferRun::magic, true),
      raw_run(parent, valid_signature, outputs, NativeTransferRun::magic, false, true),
      raw_run(parent, valid_signature, output_leaf(parent, 0, 2, 0)),
      raw_run(parent, valid_signature, output_leaf(parent, 0, 1)),
      raw_run(parent, valid_signature, output_leaf(parent, 0, 2, NativeTransferRun::outputs_leaf_magic, true)),
  };
  auto invalid_fields = parent;
  invalid_fields.src.set_zero();
  invalid_parents.push_back(raw_run(invalid_fields, valid_signature, outputs));
  invalid_fields = parent;
  invalid_fields.outputs[0].amount = 0;
  invalid_parents.push_back(raw_run(invalid_fields, valid_signature, output_leaf(invalid_fields, 0, 2)));
  invalid_fields.outputs[0].amount = std::numeric_limits<td::uint64>::max();
  invalid_parents.push_back(raw_run(invalid_fields, valid_signature, output_leaf(invalid_fields, 0, 2)));
  invalid_fields = parent;
  invalid_fields.outputs.clear();
  invalid_parents.push_back(raw_run(invalid_fields, valid_signature, outputs));
  invalid_fields.outputs.resize(17);
  invalid_parents.push_back(raw_run(invalid_fields, valid_signature, outputs));

  for (const td::uint8 version : {td::uint8{5}, td::uint8{6}}) {
    for (const auto& invalid : invalid_parents) {
      ASSERT_TRUE(CanonicalNativeTransferRun::unpack_external(invalid).is_error());
      check_rejected(header(version, 0, 2, {}, invalid));
      // A parser may already have emitted internal metadata for this good
      // first leaf. No successful partial result may escape a later failure.
      check_rejected(header(version, 0, 3, {},
                            branch(NativeTransferBatch::runs_node_magic, 1, serialize(native_metadata_test::run(1)), invalid)));
    }
  }
}

TEST(NativeParentMetadata, RejectsNoncanonicalParentOutputShape) {
  auto parent = native_metadata_test::run(3);
  const auto alternate = branch(NativeTransferRun::outputs_node_magic, 1, output_leaf(parent, 0, 1),
                                output_leaf(parent, 1, 2), 5);
  for (const td::uint8 version : {td::uint8{5}, td::uint8{6}}) {
    check_rejected(header(version, 0, 3, {}, raw_run(parent, signature(), alternate)));
    check_rejected(header(version, 0, 3, {},
                          raw_run(parent, signature(), branch(NativeTransferRun::outputs_node_magic, 0,
                                                               output_leaf(parent, 0, 1), output_leaf(parent, 1, 2), 5))));
  }
}
}  // namespace
