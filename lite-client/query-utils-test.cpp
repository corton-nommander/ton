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
