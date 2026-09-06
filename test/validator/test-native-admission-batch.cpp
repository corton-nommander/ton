#include "td/utils/tests.h"
#include "interfaces/external-message.h"

namespace {

td::Bits256 from_hex(td::Slice value) {
  td::Bits256 result;
  CHECK(result.from_hex(value) == 256);
  return result;
}

}  // namespace

TEST(NativeAdmissionBatch, DeterministicSourceNonceHashOrder) {
  auto source_a = from_hex("1000000000000000000000000000000000000000000000000000000000000000");
  auto source_b = from_hex("2000000000000000000000000000000000000000000000000000000000000000");
  auto hash_a = from_hex("0100000000000000000000000000000000000000000000000000000000000000");
  auto hash_b = from_hex("0200000000000000000000000000000000000000000000000000000000000000");

  std::vector<ton::validator::NativeAdmissionOrderKey> keys{
      {.workchain = ton::basechainId, .source = source_b, .nonce = 0, .hash = hash_a, .input_index = 4},
      {.workchain = ton::basechainId, .source = source_a, .nonce = 2, .hash = hash_a, .input_index = 1},
      {.workchain = ton::basechainId, .source = source_a, .nonce = 1, .hash = hash_b, .input_index = 2},
      {.workchain = ton::basechainId, .source = source_a, .nonce = 1, .hash = hash_a, .input_index = 3},
  };
  auto order = ton::validator::order_native_admissions(std::move(keys));
  ASSERT_EQ(order.size(), 4u);
  ASSERT_EQ(order[0], 3u);
  ASSERT_EQ(order[1], 2u);
  ASSERT_EQ(order[2], 1u);
  ASSERT_EQ(order[3], 4u);
}

TEST(NativeAdmissionBatch, StableWireStatus) {
  auto success = ton::validator::ExternalMessageAdmissionResult::success();
  ASSERT_TRUE(success.accepted);
  ASSERT_EQ(success.error_code, 0);
  ASSERT_TRUE(success.error_message.empty());

  auto failure = ton::validator::ExternalMessageAdmissionResult::failure(td::Status::Error(503, "deadline"));
  ASSERT_TRUE(!failure.accepted);
  ASSERT_EQ(failure.error_code, 503);
  ASSERT_EQ(failure.error_message, "deadline");
}
