#include "auto/tl/lite_api.hpp"
#include "block/transaction.h"
#include "crypto/Ed25519.h"
#include "query-utils.hpp"
#include "td/utils/tests.h"
#include "ton/ton-tl.hpp"
#include "vm/boc.h"

#include <limits>

namespace {

td::BufferSlice make_native_transfer_boc() {
  auto source_key = td::Ed25519::generate_private_key().move_as_ok();
  block::NativeTransfer transfer;
  transfer.src.as_slice().copy_from(source_key.get_public_key().move_as_ok().as_octet_string());
  transfer.dst = transfer.src;
  transfer.dst.as_slice()[0] = static_cast<char>(
      static_cast<unsigned char>(transfer.src.as_slice()[0]) ^ 0x80);
  transfer.amount = 1;
  transfer.valid_until = std::numeric_limits<ton::UnixTime>::max();
  // Routing only decodes the envelope; signature verification belongs to
  // admission and deliberately isn't duplicated in the client selector.
  transfer.signature.assign(64, '\x01');
  vm::CellBuilder builder;
  CHECK(transfer.store_external(builder));
  return vm::std_boc_serialize(builder.finalize()).move_as_ok();
}

struct NativeTransferRunBoc {
  td::BufferSlice boc;
  ton::StdSmcAddress source;
  std::vector<ton::StdSmcAddress> destinations;
};

NativeTransferRunBoc make_native_transfer_run_boc() {
  auto source_key = td::Ed25519::generate_private_key().move_as_ok();
  block::NativeTransferRun run;
  run.src.as_slice().copy_from(source_key.get_public_key().move_as_ok().as_octet_string());
  run.first_nonce = 7;
  run.valid_until = std::numeric_limits<ton::UnixTime>::max();
  run.signature.assign(64, '\x01');

  auto destination_a = run.src;
  auto destination_b = run.src;
  destination_b.as_slice()[0] = static_cast<char>(
      static_cast<unsigned char>(destination_b.as_slice()[0]) ^ 0x80);
  auto destination_c = run.src;
  destination_c.as_slice()[1] = static_cast<char>(
      static_cast<unsigned char>(destination_c.as_slice()[1]) ^ 0x40);
  run.outputs = {{.dst = destination_a, .amount = 1, .fee = 0},
                 {.dst = destination_b, .amount = 2, .fee = 0},
                 {.dst = destination_c, .amount = 3, .fee = 0}};

  vm::CellBuilder builder;
  CHECK(run.store_external(builder));
  return {.boc = vm::std_boc_serialize(builder.finalize()).move_as_ok(),
          .source = run.src,
          .destinations = {destination_a, destination_b, destination_c}};
}

td::BufferSlice make_malformed_native_transfer_run_boc() {
  vm::CellBuilder builder;
  CHECK(builder.store_ulong_rchk_bool(block::NativeTransferRun::magic, 32));
  return vm::std_boc_serialize(builder.finalize()).move_as_ok();
}

}  // namespace

TEST(QueryUtils, NativeTransferRoutesBothAccounts) {
  std::vector<td::BufferSlice> bodies;
  bodies.push_back(make_native_transfer_boc());
  auto query = ton::create_tl_object<ton::lite_api::liteServer_sendMessageBatch>(std::move(bodies));
  auto info = liteclient::get_query_info(*query);
  ASSERT_TRUE(info.routing_valid);
  ASSERT_EQ(info.query_id, ton::lite_api::liteServer_sendMessageBatch::ID);
  ASSERT_EQ(info.routing_shards.size(), 2u);
  for (const auto& shard : info.routing_shards) {
    ASSERT_EQ(shard.workchain, ton::basechainId);
  }
}

TEST(QueryUtils, NativeTransferRunRoutesSourceAndUniqueOutputAccounts) {
  auto run = make_native_transfer_run_boc();
  auto query = ton::create_tl_object<ton::lite_api::liteServer_sendMessage>(std::move(run.boc));
  auto info = liteclient::get_query_info(*query);
  ASSERT_TRUE(info.routing_valid);
  ASSERT_EQ(info.query_id, ton::lite_api::liteServer_sendMessage::ID);
  // The first output is deliberately the source, so it must not duplicate
  // the source routing shard.  The other two outputs have different prefixes.
  ASSERT_EQ(info.routing_shards.size(), 3u);
  ASSERT_EQ(info.routing_shards[0], ton::extract_addr_prefix(ton::basechainId, run.source).as_leaf_shard());
  ASSERT_EQ(info.routing_shards[1],
            ton::extract_addr_prefix(ton::basechainId, run.destinations[1]).as_leaf_shard());
  ASSERT_EQ(info.routing_shards[2],
            ton::extract_addr_prefix(ton::basechainId, run.destinations[2]).as_leaf_shard());
}

TEST(QueryUtils, NativeTransferRunRejectsMalformedEnvelope) {
  auto query = ton::create_tl_object<ton::lite_api::liteServer_sendMessage>(make_malformed_native_transfer_run_boc());
  auto info = liteclient::get_query_info(*query);
  ASSERT_TRUE(!info.routing_valid);
  ASSERT_TRUE(info.routing_error.find("native transfer run") != std::string::npos);
}

TEST(QueryUtils, BatchRejectsMalformedLaterBody) {
  std::vector<td::BufferSlice> bodies;
  bodies.push_back(make_native_transfer_boc());
  bodies.emplace_back(td::Slice("not-a-boc"));
  auto query = ton::create_tl_object<ton::lite_api::liteServer_sendMessageBatch>(std::move(bodies));
  auto info = liteclient::get_query_info(*query);
  ASSERT_TRUE(!info.routing_valid);
  ASSERT_TRUE(info.routing_error.find("batch body 1") != std::string::npos);
}

TEST(QueryUtils, EmptyBatchHasNoFallbackRoute) {
  auto query = ton::create_tl_object<ton::lite_api::liteServer_sendMessageBatch>(
      std::vector<td::BufferSlice>{});
  auto info = liteclient::get_query_info(*query);
  ASSERT_TRUE(!info.routing_valid);
}
