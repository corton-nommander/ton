#pragma once

#include "auto/tl/lite_api.h"
#include "block/transaction.h"
#include "Ed25519.h"
#include "td/utils/optional.h"

namespace tonlib::native_client {

struct Account {
  td::uint64 balance{0};
  td::uint64 nonce{0};
  td::uint8 flags{0};
};
struct MessageHashes {
  ton::Bits256 hash;
  ton::Bits256 hash_norm;
  bool native{false};
};
struct SignedMessage {
  td::Ref<vm::Cell> root;
  ton::StdSmcAddress source;
  ton::Bits256 hash;
};
struct PreparedBatch {
  std::vector<td::BufferSlice> bodies;
  std::vector<MessageHashes> hashes;
};
struct BatchResult {
  bool accepted{false};
  int code{0};
  std::string message;
  MessageHashes hashes;
};

// Decimal strings preserve the full uint64 range in JSON and TL clients.
td::Result<td::uint64> parse_uint64(td::Slice text, td::Slice field);
// Ordinary/absent accounts return an empty optional; malformed native cells fail.
td::Result<td::optional<Account>> decode_account(td::Ref<vm::Cell> root);
// Native hashes are the exact parent-cell hash. Ordinary TON normalization is unchanged.
td::Result<MessageHashes> message_hashes(td::Ref<vm::Cell> root);
// Pure construction: no state lookup, nonce reservation, submission, or TVM deployment.
// lane_depth=0 leaves locality validation to admission; positive depths require same-lane outputs.
td::Result<SignedMessage> create_message(const td::Ed25519::PrivateKey& key,
    std::vector<block::NativeTransferRunOutput> outputs, td::uint64 first_nonce,
    td::uint32 valid_until, const ton::Bits256& chain_domain, bool signed_run, td::uint32 lane_depth = 0);
td::Result<PreparedBatch> prepare_batch(const std::vector<std::string>& bodies);
td::Result<std::vector<BatchResult>> batch_results(const std::vector<MessageHashes>& hashes,
    const ton::lite_api::liteServer_sendMsgStatusBatch& response);

}  // namespace tonlib::native_client
