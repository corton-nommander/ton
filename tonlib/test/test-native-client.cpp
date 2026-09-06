/*
    This file is part of TON Blockchain Library.
    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.
    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library. If not, see <http://www.gnu.org/licenses/>.
*/

#include <limits>

#include "auto/tl/tonlib_api.h"
#include "auto/tl/tonlib_api_json.h"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "smc-envelope/WalletV3.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/tests.h"
#include "tl/tl_json.h"
#include "tonlib/Client.h"
#include "tonlib/native-client.h"
#include "vm/boc.h"

namespace {
namespace native = tonlib::native_client;

constexpr auto max_uint64 = std::numeric_limits<td::uint64>::max();
constexpr td::uint32 expiry = std::numeric_limits<td::uint32>::max();

ton::Bits256 filled_bits(char value) {
  ton::Bits256 result;
  result.as_slice().copy_from(std::string(32, value));
  return result;
}

td::Ed25519::PrivateKey private_key() {
  return td::Ed25519::PrivateKey(td::SecureString(std::string(32, '\x42')));
}

ton::StdSmcAddress public_address() {
  ton::StdSmcAddress result;
  result.as_slice().copy_from(private_key().get_public_key().move_as_ok().as_octet_string());
  return result;
}

td::Ref<vm::Cell> small_body() {
  vm::CellBuilder cb;
  CHECK(cb.store_ulong_rchk_bool(0x6ad, 11));
  return cb.finalize();
}

// A separate wire-format fixture provides the normalization oracle: external
// source/import fee/StateInit are discarded, while destination/body are kept.
td::Ref<vm::Cell> ordinary_message(td::Ref<vm::Cell> body, bool canonical) {
  vm::CellBuilder cb;
  CHECK(cb.store_ulong_rchk_bool(2, 2));  // ext_in_msg_info$10
  if (canonical) {
    CHECK(cb.store_ulong_rchk_bool(0, 2));  // addr_none
  } else {
    CHECK(cb.store_ulong_rchk_bool(1, 2) && cb.store_ulong_rchk_bool(12, 9) &&
          cb.store_ulong_rchk_bool(0xabc, 12));  // addr_extern
  }
  CHECK(cb.store_ulong_rchk_bool(2, 2) && cb.store_ulong_rchk_bool(0, 1) &&
        cb.store_ulong_rchk_bool(0, 8) && cb.store_bits_bool(filled_bits('\x31')));
  if (canonical) {
    CHECK(cb.store_ulong_rchk_bool(0, 4) && cb.store_ulong_rchk_bool(0, 1) &&
          cb.store_ulong_rchk_bool(1, 1) && cb.store_ref_bool(body));
  } else {
    auto empty_state = vm::CellBuilder().store_zeroes(5).finalize();
    CHECK(cb.store_ulong_rchk_bool(1, 4) && cb.store_ulong_rchk_bool(7, 8) &&
          cb.store_ulong_rchk_bool(3, 2) && cb.store_ref_bool(empty_state) &&
          cb.store_ulong_rchk_bool(0, 1) && cb.append_cellslice_bool(vm::load_cell_slice(body)));
  }
  auto root = cb.finalize();
  CHECK(block::gen::t_Message_Any.validate_ref(root));
  return root;
}

td::Ref<vm::Cell> compact_account(td::uint64 balance, td::uint64 nonce, td::uint8 flags) {
  vm::CellBuilder cb;
  CHECK(cb.store_ulong_rchk_bool(1, 2) && cb.store_ulong_rchk_bool(balance, 64) &&
        cb.store_ulong_rchk_bool(nonce, 64) && cb.store_ulong_rchk_bool(flags, 8));
  return cb.finalize();
}

// A complete ordinary active account, including its independent code/data.
td::Ref<vm::Cell> ordinary_account() {
  vm::CellBuilder cb;
  CHECK(cb.store_ulong_rchk_bool(1, 1) && cb.store_ulong_rchk_bool(2, 2) &&
        cb.store_ulong_rchk_bool(0, 1) && cb.store_ulong_rchk_bool(0, 8) &&
        cb.store_bits_bool(filled_bits('\x31')) && cb.store_ulong_rchk_bool(0, 6) &&
        cb.store_ulong_rchk_bool(0, 3) && cb.store_ulong_rchk_bool(0, 32) &&
        cb.store_ulong_rchk_bool(0, 1) && cb.store_ulong_rchk_bool(0, 64) &&
        cb.store_ulong_rchk_bool(0, 5) && cb.store_ulong_rchk_bool(1, 1) &&
        cb.store_ulong_rchk_bool(0, 2) && cb.store_ulong_rchk_bool(1, 1) &&
        cb.store_ref_bool(small_body()) && cb.store_ulong_rchk_bool(1, 1) &&
        cb.store_ref_bool(small_body()) && cb.store_ulong_rchk_bool(0, 1));
  auto root = cb.finalize();
  CHECK(block::gen::t_Account.validate_ref(root));
  return root;
}

std::string boc(td::Ref<vm::Cell> root) {
  return vm::std_boc_serialize(root).move_as_ok().as_slice().str();
}

native::SignedMessage signed_message(bool run, td::uint64 nonce = 19) {
  return native::create_message(private_key(), {{filled_bits('\x31'), 123, 7}}, nonce,
                                expiry, filled_bits('\x51'), run).move_as_ok();
}

}  // namespace

TEST(TonlibNative, DecimalUint64Bounds) {
  ASSERT_EQ(native::parse_uint64("0", "balance").move_as_ok(), 0u);
  ASSERT_EQ(native::parse_uint64("000019", "nonce").move_as_ok(), 19u);
  ASSERT_EQ(native::parse_uint64("9223372036854775808", "balance").move_as_ok(), td::uint64{1} << 63);
  ASSERT_EQ(native::parse_uint64("18446744073709551615", "nonce").move_as_ok(), max_uint64);
  for (const char* value : {"", "-1", "+1", " 1", "1 ", "1.0", "1e2", "0x10",
                          "18446744073709551616", "9999999999999999999999999999999"}) {
    auto result = native::parse_uint64(td::Slice(value), "nonce");
    ASSERT_TRUE(result.is_error());
    ASSERT_TRUE(result.error().message().str().find("nonce") != std::string::npos);
  }
  ASSERT_TRUE(native::parse_uint64(td::Slice("1\0", 2), "nonce").is_error());
}

TEST(TonlibNative, AccountDecodingPreservesUint64AndOrdinaryAccounts) {
  for (auto value : {td::uint64{0}, td::uint64{1} << 63, max_uint64}) {
    auto root = compact_account(value, value, 255);
    ASSERT_TRUE(block::gen::t_Account.validate_ref(root));
    auto decoded = native::decode_account(root).move_as_ok();
    ASSERT_TRUE(static_cast<bool>(decoded));
    ASSERT_EQ(decoded.value().balance, value);
    ASSERT_EQ(decoded.value().nonce, value);
    ASSERT_EQ(decoded.value().flags, 255);
  }
  ASSERT_TRUE(!native::decode_account({}).move_as_ok());
  ASSERT_TRUE(!native::decode_account(vm::CellBuilder().store_zeroes(2).finalize()).move_as_ok());
  ASSERT_TRUE(!native::decode_account(ordinary_account()).move_as_ok());

  auto truncated = vm::CellBuilder().store_long(1, 2).finalize();
  ASSERT_TRUE(native::decode_account(truncated).is_error());
  auto valid = compact_account(7, 9, 3);
  auto trailing_bit = vm::CellBuilder().append_cellslice(vm::load_cell_slice(valid)).store_ones(1).finalize();
  auto trailing_ref = vm::CellBuilder().append_cellslice(vm::load_cell_slice(valid)).store_ref(small_body()).finalize();
  ASSERT_TRUE(native::decode_account(trailing_bit).is_error());
  ASSERT_TRUE(native::decode_account(trailing_ref).is_error());
}

TEST(TonlibNative, OrdinaryMessageNormalizationIsUnchanged) {
  auto canonical = ordinary_message(small_body(), true);
  auto original = ordinary_message(small_body(), false);
  ASSERT_TRUE(original->get_hash() != canonical->get_hash());
  auto hashes = native::message_hashes(original).move_as_ok();
  ASSERT_TRUE(!hashes.native);
  ASSERT_EQ(hashes.hash, original->get_hash().bits());
  ASSERT_EQ(hashes.hash_norm, canonical->get_hash().bits());
  auto canonical_hashes = native::message_hashes(canonical).move_as_ok();
  ASSERT_EQ(canonical_hashes.hash, canonical_hashes.hash_norm);
  ASSERT_TRUE(native::message_hashes({}).is_error());
  ASSERT_TRUE(native::message_hashes(small_body()).is_error());
}

TEST(TonlibNative, TransferHashesAndSignaturesUseExactParentAndDomain) {
  auto signed_transfer = signed_message(false);
  auto transfer = block::NativeTransfer::unpack_external(signed_transfer.root).move_as_ok();
  ASSERT_EQ(signed_transfer.source, public_address());
  ASSERT_EQ(transfer.src, signed_transfer.source);
  ASSERT_EQ(transfer.dst, filled_bits('\x31'));
  ASSERT_EQ(transfer.amount, 123u);
  ASSERT_EQ(transfer.fee, 7u);
  ASSERT_EQ(transfer.nonce, 19u);
  ASSERT_EQ(transfer.valid_until, expiry);
  ASSERT_TRUE(transfer.verify_signature(filled_bits('\x51')).is_ok());
  ASSERT_TRUE(transfer.verify_signature(filled_bits('\x52')).is_error());
  ASSERT_TRUE(transfer.verify_signature().is_error());
  auto hashes = native::message_hashes(signed_transfer.root).move_as_ok();
  ASSERT_TRUE(hashes.native);
  ASSERT_EQ(hashes.hash, signed_transfer.hash);
  ASSERT_EQ(hashes.hash_norm, transfer.external_hash().move_as_ok());
  ASSERT_EQ(hashes.hash, hashes.hash_norm);
  ASSERT_EQ(native::message_hashes(vm::std_boc_deserialize(boc(signed_transfer.root)).move_as_ok())
                .move_as_ok().hash, hashes.hash);
  transfer.amount++;
  ASSERT_TRUE(transfer.verify_signature(filled_bits('\x51')).is_error());
}

TEST(TonlibNative, RunAuthorizationPreservesOrderAndNonceRange) {
  std::vector<block::NativeTransferRunOutput> outputs;
  for (std::size_t i = 0; i < block::NativeTransferRun::max_entries; ++i) {
    outputs.push_back({filled_bits(static_cast<char>(i + 1)), i + 1, i});
  }
  auto message = native::create_message(private_key(), outputs, max_uint64 - outputs.size(), expiry,
                                        filled_bits('\x51'), true).move_as_ok();
  auto run = block::NativeTransferRun::unpack_external(message.root).move_as_ok();
  ASSERT_EQ(run.first_nonce, max_uint64 - outputs.size());
  ASSERT_EQ(run.outputs.size(), outputs.size());
  ASSERT_EQ(run.src, public_address());
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    ASSERT_EQ(run.outputs[i].dst, outputs[i].dst);
    ASSERT_EQ(run.outputs[i].amount, outputs[i].amount);
    ASSERT_EQ(run.outputs[i].fee, outputs[i].fee);
  }
  ASSERT_TRUE(run.verify_signature(filled_bits('\x51')).is_ok());
  ASSERT_TRUE(run.verify_signature(filled_bits('\x52')).is_error());
  ASSERT_TRUE(run.verify_signature().is_error());
  auto hashes = native::message_hashes(message.root).move_as_ok();
  ASSERT_TRUE(hashes.native);
  ASSERT_EQ(hashes.hash, message.hash);
  ASSERT_EQ(hashes.hash_norm, run.external_hash().move_as_ok());
  ASSERT_EQ(hashes.hash, hashes.hash_norm);
  std::swap(run.outputs[0], run.outputs[1]);
  ASSERT_TRUE(run.verify_signature(filled_bits('\x51')).is_error());
}

TEST(TonlibNative, ConstructionRejectsInvalidAmountsNoncesAndLanes) {
  const auto domain = filled_bits('\x51');
  const std::vector<block::NativeTransferRunOutput> one{{public_address(), 1, 0}};
  const auto make = [&](std::vector<block::NativeTransferRunOutput> outputs, td::uint64 nonce,
                        bool run, td::uint32 depth = 0, td::uint32 until = expiry) {
    return native::create_message(private_key(), std::move(outputs), nonce, until, domain, run, depth);
  };
  ASSERT_TRUE(make({}, 0, true).is_error());
  ASSERT_TRUE(make(std::vector<block::NativeTransferRunOutput>(17, one[0]), 0, true).is_error());
  ASSERT_TRUE(make(std::vector<block::NativeTransferRunOutput>(2, one[0]), 0, false).is_error());
  ASSERT_TRUE(make(one, max_uint64, false).is_error());
  ASSERT_TRUE(make(one, max_uint64 - 1, false).is_ok());
  ASSERT_TRUE(make(std::vector<block::NativeTransferRunOutput>(16, one[0]), max_uint64 - 15, true).is_error());
  ASSERT_TRUE(make({{public_address(), 0, 0}}, 0, false).is_error());
  ASSERT_TRUE(make({{public_address(), max_uint64, 1}}, 0, true).is_error());
  ASSERT_TRUE(make({{public_address(), max_uint64, 0}}, 0, true).is_ok());
  ASSERT_TRUE(make(one, 0, true, 0, 0).is_error());
  ASSERT_TRUE(make(one, 0, true, 2).is_ok());
  ASSERT_TRUE(make(one, 0, true, block::NativePaymentLanePolicy::max_depth + 1).is_error());
  auto other_lane = public_address();
  other_lane.as_slice()[0] ^= '\x80';
  ASSERT_TRUE(make({{other_lane, 1, 0}}, 0, true, 2).is_error());
  ASSERT_TRUE(make({{other_lane, 1, 0}}, 0, false, 2).is_error());
  ASSERT_TRUE(make({{other_lane, 1, 0}}, 0, true, 0).is_ok());
}

TEST(TonlibNative, MalformedNativeParentsCannotAcquireHashes) {
  for (auto magic : {block::NativeTransfer::magic, block::NativeTransferRun::magic}) {
    auto truncated = vm::CellBuilder().store_long(magic, 32).finalize();
    ASSERT_TRUE(native::message_hashes(truncated).is_error());
  }
  for (bool run : {false, true}) {
    auto root = signed_message(run).root;
    auto trailing = vm::CellBuilder().append_cellslice(vm::load_cell_slice(root)).store_ones(1).finalize();
    ASSERT_TRUE(native::message_hashes(trailing).is_error());
  }
}

TEST(TonlibNative, BatchPreparationPreservesMixedParentOrderAndBounds) {
  std::vector<std::string> bodies{boc(signed_message(true).root), boc(ordinary_message(small_body(), false)),
                                 boc(signed_message(false, 20).root)};
  auto batch = native::prepare_batch(bodies).move_as_ok();
  ASSERT_EQ(batch.bodies.size(), bodies.size());
  ASSERT_EQ(batch.hashes.size(), bodies.size());
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    ASSERT_EQ(batch.bodies[i].as_slice(), td::Slice(bodies[i]));
    auto root = vm::std_boc_deserialize(bodies[i]).move_as_ok();
    ASSERT_EQ(batch.hashes[i].hash, root->get_hash().bits());
    ASSERT_EQ(batch.hashes[i].native, i != 1);
  }
  ASSERT_TRUE(native::prepare_batch({}).is_error());
  ASSERT_TRUE(native::prepare_batch(std::vector<std::string>(1024, bodies[0])).is_ok());
  ASSERT_TRUE(native::prepare_batch(std::vector<std::string>(1025, bodies[0])).is_error());
  auto malformed = bodies;
  malformed[1] = "not a BOC";
  auto invalid_boc = native::prepare_batch(malformed);
  ASSERT_TRUE(invalid_boc.is_error());
  ASSERT_TRUE(invalid_boc.error().message().str().find("index 1") != std::string::npos);
  malformed[1] = boc(small_body());
  auto invalid_message = native::prepare_batch(malformed);
  ASSERT_TRUE(invalid_message.is_error());
  ASSERT_TRUE(invalid_message.error().message().str().find("index 1") != std::string::npos);

  // Valid cells/BOCs exceed the byte limit with a legal item count; a malformed
  // payload would only prove that BOC parsing fails, not that this bound works.
  auto large_body = small_body();
  for (unsigned i = 0; i < 72; ++i) {
    vm::CellBuilder cb;
    CHECK(cb.store_bytes_bool(std::string(120, static_cast<char>(i))) && cb.store_ref_bool(large_body));
    large_body = cb.finalize();
  }
  auto large_boc = boc(ordinary_message(large_body, true));
  auto count = (8u << 20) / large_boc.size() + 1;
  ASSERT_TRUE(count <= 1024);
  auto oversized = native::prepare_batch(std::vector<std::string>(count, large_boc));
  ASSERT_TRUE(oversized.is_error());
  ASSERT_TRUE(oversized.error().message().str().find("8 MiB") != std::string::npos);
}

TEST(TonlibNative, BatchResultsPreserveOrderedPartialFailures) {
  auto prepared = native::prepare_batch({boc(signed_message(true).root), boc(signed_message(false).root),
                                         boc(ordinary_message(small_body(), false))}).move_as_ok();
  ton::lite_api::liteServer_sendMsgStatusBatch response;
  response.results_.push_back(ton::lite_api::make_object<ton::lite_api::liteServer_sendMsgResult>(1, 0, ""));
  response.results_.push_back(ton::lite_api::make_object<ton::lite_api::liteServer_sendMsgResult>(0, -400, "bad signature"));
  response.results_.push_back(ton::lite_api::make_object<ton::lite_api::liteServer_sendMsgResult>(1, 0, ""));
  auto results = native::batch_results(prepared.hashes, response).move_as_ok();
  ASSERT_EQ(results.size(), 3u);
  for (std::size_t i = 0; i < results.size(); ++i) {
    ASSERT_EQ(results[i].accepted, i != 1);
    ASSERT_EQ(results[i].hashes.hash, prepared.hashes[i].hash);
    ASSERT_EQ(results[i].hashes.hash_norm, prepared.hashes[i].hash_norm);
    ASSERT_EQ(results[i].hashes.native, prepared.hashes[i].native);
  }
  ASSERT_EQ(results[1].code, -400);
  ASSERT_EQ(results[1].message, "bad signature");
  response.results_[0]->status_ = 2;
  ASSERT_TRUE(native::batch_results(prepared.hashes, response).is_error());
  response.results_[0]->status_ = 1;
  response.results_[0]->code_ = 7;
  ASSERT_TRUE(native::batch_results(prepared.hashes, response).is_error());
  response.results_[0]->code_ = 0;
  response.results_[0]->message_ = "unexpected success error";
  ASSERT_TRUE(native::batch_results(prepared.hashes, response).is_error());
  response.results_[0] = nullptr;
  ASSERT_TRUE(native::batch_results(prepared.hashes, response).is_error());
  response.results_.pop_back();
  ASSERT_TRUE(native::batch_results(prepared.hashes, response).is_error());
}


TEST(TonlibNative, NativeAccountJsonPreservesUnsignedDecimalStrings) {
  auto state = tonlib_api::make_object<tonlib_api::native_accountState>(
      "18446744073709551615", "9223372036854775808", 255);
  auto json = td::json_encode<std::string>(td::ToJson(static_cast<const tonlib_api::Object&>(*state)));
  auto decoded = td::json_decode(json).move_as_ok();
  auto& object = decoded.get_object();
  auto balance = object.extract_required_field("balance", td::JsonValue::Type::String).move_as_ok();
  auto nonce = object.extract_required_field("nonce", td::JsonValue::Type::String).move_as_ok();
  ASSERT_EQ(balance.get_string(), "18446744073709551615");
  ASSERT_EQ(nonce.get_string(), "9223372036854775808");
  ASSERT_EQ(native::parse_uint64(balance.get_string(), "balance").move_as_ok(), max_uint64);
  ASSERT_EQ(native::parse_uint64(nonce.get_string(), "nonce").move_as_ok(), td::uint64{1} << 63);
}

TEST(TonlibNative, PublicAddressApiKeepsNativeKeysAndOrdinaryWalletsDistinct) {
  const auto key_text = block::PublicKey::from_bytes(public_address().as_slice()).move_as_ok().serialize();
  auto native_response = tonlib::Client::execute(
      {41, tonlib_api::make_object<tonlib_api::native_getAccountAddress>(key_text)});
  ASSERT_EQ(native_response.id, 41u);
  ASSERT_TRUE(native_response.object != nullptr);
  ASSERT_EQ(native_response.object->get_id(), tonlib_api::accountAddress::ID);
  auto native_address = block::StdAddress::parse(
      tonlib_api::move_object_as<tonlib_api::accountAddress>(native_response.object)->account_address_).move_as_ok();
  ASSERT_EQ(native_address.workchain, ton::basechainId);
  ASSERT_EQ(native_address.addr, public_address());

  auto wallet_response = tonlib::Client::execute(
      {42, tonlib_api::make_object<tonlib_api::getAccountAddress>(
               tonlib_api::make_object<tonlib_api::wallet_v3_initialAccountState>(key_text, 123), 0, -1)});
  ASSERT_EQ(wallet_response.id, 42u);
  ASSERT_TRUE(wallet_response.object != nullptr);
  ASSERT_EQ(wallet_response.object->get_id(), tonlib_api::accountAddress::ID);
  auto wallet_address = block::StdAddress::parse(
      tonlib_api::move_object_as<tonlib_api::accountAddress>(wallet_response.object)->account_address_).move_as_ok();
  auto wallet = ton::WalletV3::create({td::SecureString(public_address().as_slice()), 123}, 0);
  ASSERT_TRUE(wallet_address == wallet->get_address(-1));
  ASSERT_EQ(wallet_address.workchain, ton::masterchainId);
  ASSERT_TRUE(wallet_address.addr != native_address.addr);

  for (const char* invalid : {"", "not an Ed25519 public key"}) {
    auto response = tonlib::Client::execute(
        {43, tonlib_api::make_object<tonlib_api::native_getAccountAddress>(std::string(invalid))});
    ASSERT_TRUE(response.object != nullptr);
    ASSERT_EQ(response.object->get_id(), tonlib_api::error::ID);
  }
}

TEST(TonlibNative, WalletConstructionBoundsAggregateDebitButRawBatchPreservesAdmissionFailures) {
  const auto destination = public_address();
  const auto domain = filled_bits('\x51');
  auto make = [&](std::vector<block::NativeTransferRunOutput> outputs) {
    return native::create_message(private_key(), std::move(outputs), 0, expiry, domain, true);
  };
  // Every output independently fits uint64; the complete source authorization
  // cannot debit more than its representable native balance.
  ASSERT_TRUE(make({{destination, max_uint64, 0}, {destination, 1, 0}}).is_error());
  ASSERT_TRUE(make({{destination, 1, max_uint64 - 1}, {destination, 1, 2}}).is_error());
  ASSERT_TRUE(make({{destination, max_uint64 - 2, 1}, {destination, 1, 1}}).is_error());
  ASSERT_TRUE(make({{destination, max_uint64 - 2, 0}, {destination, 1, 1}}).is_ok());

  // Raw submission accepts structurally valid protocol parents so an admission
  // rejection remains one ordered result alongside the other submitted items.
  block::NativeTransferRun run;
  run.src = public_address();
  run.first_nonce = 0;
  run.valid_until = expiry;
  run.outputs = {{destination, max_uint64, 0}, {destination, 1, 0}};
  run.signature = private_key().sign(run.signing_payload(domain)).move_as_ok().as_slice().str();
  vm::CellBuilder cb;
  ASSERT_TRUE(run.store_external(cb));
  auto root = cb.finalize();
  auto prepared = native::prepare_batch({boc(root), boc(signed_message(true).root)}).move_as_ok();
  ASSERT_EQ(prepared.hashes.size(), 2u);
  ASSERT_EQ(prepared.hashes[0].hash, root->get_hash().bits());
}
