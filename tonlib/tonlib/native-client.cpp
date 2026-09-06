#include "tonlib/native-client.h"

#include <limits>

#include "block/block-parse.h"
#include "tl/tlblib.hpp"
#include "vm/excno.hpp"

namespace tonlib::native_client {

td::Result<td::uint64> parse_uint64(td::Slice text, td::Slice field) {
  if (text.empty()) {
    return td::Status::Error(PSLICE() << field << " must be an unsigned decimal uint64 string");
  }
  td::uint64 value = 0;
  for (char c : text) {
    if (c < '0' || c > '9' || value > (std::numeric_limits<td::uint64>::max() - (c - '0')) / 10) {
      return td::Status::Error(PSLICE() << field << " must be an unsigned decimal uint64 string");
    }
    value = value * 10 + (c - '0');
  }
  return value;
}

static td::Result<td::optional<Account>> decode_account_impl(td::Ref<vm::Cell> root) {
  if (root.is_null()) {
    return td::optional<Account>{};
  }
  auto cs = vm::load_cell_slice(root);
  if (block::gen::t_Account.get_tag(cs) != block::gen::Account::account_native) {
    return td::optional<Account>{};
  }
  block::gen::Account::Record_account_native native;
  if (!tlb::unpack_exact(cs, native)) {
    return td::Status::Error("Failed to unpack native Account");
  }
  return td::optional<Account>{Account{native.balance, native.nonce, static_cast<td::uint8>(native.flags)}};
}

static td::Result<MessageHashes> message_hashes_impl(td::Ref<vm::Cell> root) {
  if (root.is_null()) {
    return td::Status::Error("Message root is missing");
  }
  MessageHashes hashes{root->get_hash().bits(), root->get_hash().bits(), false};
  auto cs = vm::load_cell_slice(root);
  auto tag = cs.prefetch_ulong(32);
  if (tag == block::NativeTransfer::magic) {
    TRY_RESULT(transfer, block::NativeTransfer::unpack_external(root));
    hashes.native = true;
    return hashes;
  }
  if (tag == block::NativeTransferRun::magic) {
    TRY_RESULT(run, block::NativeTransferRun::unpack_external(root));
    hashes.native = true;
    return hashes;
  }
  block::gen::Message::Record message;
  if (!tlb::type_unpack_cell(root, block::gen::t_Message_Any, message)) {
    return td::Status::Error("Failed to unpack Message");
  }
  if (block::gen::CommonMsgInfo().get_tag(*message.info) != block::gen::CommonMsgInfo::ext_in_msg_info) {
    return td::Status::Error("CommonMsgInfo tag is not ext_in_msg_info");
  }
  block::gen::CommonMsgInfo::Record_ext_in_msg_info msg_info;
  if (!tlb::csr_unpack(message.info, msg_info)) {
    return td::Status::Error("Failed to unpack CommonMsgInfo::ext_in_msg_info");
  }
  td::Ref<vm::Cell> body;
  auto body_cs = message.body.write();
  if (body_cs.fetch_ulong(1) == 1) {
    body = body_cs.fetch_ref();
  } else {
    body = vm::CellBuilder().append_cellslice(body_cs).finalize();
  }
  vm::CellBuilder cb;
  if (!(cb.store_long_bool(2, 2) && cb.store_long_bool(0, 2) && cb.append_cellslice_bool(msg_info.dest) &&
        cb.store_long_bool(0, 4) && cb.store_long_bool(0, 1) && cb.store_long_bool(1, 1) && cb.store_ref_bool(body))) {
    return td::Status::Error("Failed to build normalized message");
  }
  hashes.hash_norm = cb.finalize()->get_hash().bits();
  return hashes;
}

td::Result<td::optional<Account>> decode_account(td::Ref<vm::Cell> root) {
  return TRY_VM(decode_account_impl(std::move(root)));
}

td::Result<MessageHashes> message_hashes(td::Ref<vm::Cell> root) {
  return TRY_VM(message_hashes_impl(std::move(root)));
}

td::Result<SignedMessage> create_message(const td::Ed25519::PrivateKey& key,
    std::vector<block::NativeTransferRunOutput> outputs, td::uint64 first_nonce,
    td::uint32 valid_until, const ton::Bits256& chain_domain, bool signed_run, td::uint32 lane_depth) {
  if (outputs.empty() || outputs.size() > block::NativeTransferRun::max_entries ||
      (!signed_run && outputs.size() != 1)) {
    return td::Status::Error("Native transfer requires one output; a signed run requires 1..16 outputs");
  }
  // Execution consumes the final nonce as well, so UINT64_MAX is never usable.
  if (first_nonce > std::numeric_limits<td::uint64>::max() - outputs.size()) {
    return td::Status::Error("Native transfer nonce interval overflows uint64");
  }
  if (!valid_until) {
    return td::Status::Error("Native transfer valid_until must be positive");
  }
  TRY_RESULT(public_key, key.get_public_key());
  SignedMessage result;
  result.source.as_slice().copy_from(public_key.as_octet_string());
  td::optional<block::NativePaymentLanePolicy> lanes;
  if (lane_depth) {
    TRY_RESULT(policy, block::NativePaymentLanePolicy::from_fixed_split_depth(lane_depth, lane_depth));
    lanes = std::move(policy);
  }
  td::uint64 aggregate_debit = 0;
  for (const auto& output : outputs) {
    if (!output.amount || output.fee > std::numeric_limits<td::uint64>::max() - output.amount) {
      return td::Status::Error("Native transfer amount must be positive and amount plus fee must fit uint64");
    }
    const auto debit = output.amount + output.fee;
    if (debit > std::numeric_limits<td::uint64>::max() - aggregate_debit) {
      return td::Status::Error("Native transfer run aggregate debit overflows uint64");
    }
    aggregate_debit += debit;
    if (lanes && !lanes.value().contains(result.source, output.dst)) {
      return td::Status::Error("Native transfer destination is outside the requested payment lane");
    }
  }
  vm::CellBuilder cb;
  if (signed_run) {
    block::NativeTransferRun run;
    run.src = result.source;
    run.first_nonce = first_nonce;
    run.valid_until = valid_until;
    run.outputs = std::move(outputs);
    TRY_RESULT(signature, key.sign(run.signing_payload(chain_domain)));
    run.signature = signature.as_slice().str();
    if (!run.store_external(cb)) {
      return td::Status::Error("Failed to serialize native transfer run");
    }
  } else {
    block::NativeTransfer transfer;
    transfer.src = result.source;
    transfer.dst = outputs[0].dst;
    transfer.amount = outputs[0].amount;
    transfer.fee = outputs[0].fee;
    transfer.nonce = first_nonce;
    transfer.valid_until = valid_until;
    TRY_RESULT(signature, key.sign(transfer.signing_payload(chain_domain)));
    transfer.signature = signature.as_slice().str();
    if (!transfer.store_external(cb)) {
      return td::Status::Error("Failed to serialize native transfer");
    }
  }
  result.root = cb.finalize();
  result.hash = result.root->get_hash().bits();
  return result;
}

td::Result<PreparedBatch> prepare_batch(const std::vector<std::string>& bodies) {
  constexpr std::size_t max_bytes = 8 << 20;
  if (bodies.empty() || bodies.size() > 1024) {
    return td::Status::Error("sendMessageBatch requires 1..1024 bodies");
  }
  std::size_t bytes = 0;
  for (const auto& body : bodies) {
    if (body.size() > max_bytes - bytes) {
      return td::Status::Error("sendMessageBatch payload exceeds 8 MiB");
    }
    bytes += body.size();
  }
  PreparedBatch result;
  result.bodies.reserve(bodies.size());
  result.hashes.reserve(bodies.size());
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    TRY_RESULT_PREFIX(root, vm::std_boc_deserialize(td::Slice(bodies[i])),
                      PSLICE() << "Invalid batch BOC at index " << i << ": ");
    TRY_RESULT_PREFIX(hashes, message_hashes(root), PSLICE() << "Invalid batch message at index " << i << ": ");
    result.bodies.emplace_back(td::Slice(bodies[i]));
    result.hashes.push_back(std::move(hashes));
  }
  return result;
}

td::Result<std::vector<BatchResult>> batch_results(const std::vector<MessageHashes>& hashes,
    const ton::lite_api::liteServer_sendMsgStatusBatch& response) {
  if (response.results_.size() != hashes.size()) {
    return td::Status::Error("sendMessageBatch result count mismatch");
  }
  std::vector<BatchResult> results;
  results.reserve(hashes.size());
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    const auto& status = response.results_[i];
    if (!status || (status->status_ != 0 && status->status_ != 1) ||
        (status->status_ == 1 && (status->code_ != 0 || !status->message_.empty()))) {
      return td::Status::Error(PSLICE() << "Invalid sendMessageBatch status at index " << i);
    }
    results.push_back(BatchResult{status->status_ == 1, status->code_, status->message_, hashes[i]});
  }
  return results;
}

}  // namespace tonlib::native_client
