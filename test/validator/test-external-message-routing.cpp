#include <limits>

#include "crypto/Ed25519.h"
#include "td/utils/tests.h"
#include "validator/impl/external-message.hpp"
#include "vm/boc.h"

namespace {

td::BufferSlice make_native_transfer_run_boc() {
  auto source_key = td::Ed25519::generate_private_key().move_as_ok();
  block::NativeTransferRun run;
  run.src.as_slice().copy_from(source_key.get_public_key().move_as_ok().as_octet_string());
  run.first_nonce = 3;
  run.valid_until = std::numeric_limits<ton::UnixTime>::max();
  run.signature.assign(64, '\x01');
  run.outputs = {{.dst = run.src, .amount = 1, .fee = 0}};

  vm::CellBuilder builder;
  CHECK(run.store_external(builder));
  return vm::std_boc_serialize(builder.finalize()).move_as_ok();
}

td::BufferSlice make_malformed_native_transfer_run_boc() {
  vm::CellBuilder builder;
  CHECK(builder.store_ulong_rchk_bool(block::NativeTransferRun::magic, 32));
  return vm::std_boc_serialize(builder.finalize()).move_as_ok();
}

}  // namespace

TEST(ExternalMessageRouting, NativeTransferRunRoutesToItsSourceForCapabilityGatedAdmission) {
  auto result = ton::validator::ExtMessageQ::create_ext_message(
      make_native_transfer_run_boc(), block::SizeLimitsConfig::ExtMsgLimits{});
  ASSERT_TRUE(result.is_ok());
  auto message = result.move_as_ok();
  ASSERT_EQ(message->wc(), ton::basechainId);
  ASSERT_TRUE(message->root_cell().not_null());
  ASSERT_EQ(vm::load_cell_slice(message->root_cell()).prefetch_ulong(32), block::NativeTransferRun::magic);
}

TEST(ExternalMessageRouting, MalformedNativeTransferRunDoesNotFallBackToOrdinaryExternalMessage) {
  auto result = ton::validator::ExtMessageQ::create_ext_message(
      make_malformed_native_transfer_run_boc(), block::SizeLimitsConfig::ExtMsgLimits{});
  ASSERT_TRUE(result.is_error());
  ASSERT_TRUE(result.error().message().str().find("native transfer run") != std::string::npos);
}
