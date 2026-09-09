#pragma once

#include <algorithm>
#include <limits>
#include <string>

#include "block/transaction.h"
#include "td/utils/logging.h"
#include "vm/boc.h"

namespace native_metadata_test {

inline ton::StdSmcAddress address(td::uint64 index, unsigned prefix = 0x11) {
  ton::StdSmcAddress result;
  std::string bytes(32, static_cast<char>(prefix));
  for (unsigned byte = 0; byte < 8; ++byte) {
    bytes[31 - byte] = static_cast<char>(index >> (8 * byte));
  }
  result.as_slice().copy_from(bytes);
  return result;
}

// Deliberately not cryptographically signed: neither batch decoder performs
// signature authentication. These fixtures exercise the strict wire codec.
inline block::NativeTransferRun run(std::size_t count, td::uint64 nonce = 100, td::uint64 source = 1) {
  block::NativeTransferRun result;
  result.src = address(source);
  result.first_nonce = nonce;
  result.valid_until = std::numeric_limits<ton::UnixTime>::max();
  result.signature.assign(64, '\x5a');
  for (std::size_t index = 0; index < count; ++index) {
    result.outputs.push_back({address(source * 16 + index, 0x22), index + 1, 3});
  }
  CHECK(result.is_valid());
  return result;
}

inline block::NativeTransfer scalar(td::uint64 nonce, td::uint64 source = 1) {
  const auto parent = run(1, nonce, source);
  block::NativeTransfer result;
  result.src = parent.src;
  result.dst = parent.outputs[0].dst;
  result.amount = parent.outputs[0].amount;
  result.fee = parent.outputs[0].fee;
  result.nonce = nonce;
  result.valid_until = parent.valid_until;
  result.signature = parent.signature;
  return result;
}

inline td::Ref<vm::Cell> serialize(const block::NativeTransferRun& value) {
  vm::CellBuilder builder;
  CHECK(value.store_external(builder));
  return builder.finalize();
}

inline td::Ref<vm::Cell> serialize(const block::NativeTransferBatch& value) {
  vm::CellBuilder builder;
  CHECK(value.store(builder));
  return builder.finalize();
}

inline block::NativeTransferBatch direct_batch(td::uint8 version, std::size_t logical_count,
                                               std::size_t quantum = 16) {
  CHECK(quantum >= 1 && quantum <= block::NativeTransferRun::max_entries);
  CHECK(logical_count <= block::NativeTransferBatch::max_entries);
  block::NativeTransferBatch batch;
  batch.version = version;
  for (std::size_t index = 0; index < logical_count; index += quantum) {
    batch.runs.push_back(run(std::min(quantum, logical_count - index), index, index / quantum + 1));
  }
  return batch;
}

inline td::Ref<vm::Cell> header(td::uint8 version, td::uint32 accounts, td::uint32 entries,
                               td::Ref<vm::Cell> account_root = {}, td::Ref<vm::Cell> entry_root = {},
                               td::uint32 magic = block::NativeTransferBatch::magic, bool trailing_bit = false) {
  vm::CellBuilder builder;
  CHECK(builder.store_ulong_rchk_bool(magic, 32));
  CHECK(builder.store_ulong_rchk_bool(version, 8));
  CHECK(builder.store_ulong_rchk_bool(accounts, 32));
  CHECK(builder.store_ulong_rchk_bool(entries, 32));
  CHECK(builder.store_maybe_ref(account_root));
  CHECK(builder.store_maybe_ref(entry_root));
  CHECK(!trailing_bit || builder.store_bool_bool(true));
  return builder.finalize();
}

inline td::Ref<vm::Cell> branch(td::uint32 magic, td::uint32 left_count, td::Ref<vm::Cell> left,
                               td::Ref<vm::Cell> right, unsigned count_bits = 32, bool trailing_bit = false) {
  vm::CellBuilder builder;
  CHECK(builder.store_ulong_rchk_bool(magic, 32));
  CHECK(builder.store_ulong_rchk_bool(left_count, count_bits));
  CHECK(left.is_null() || builder.store_ref_bool(left));
  CHECK(right.is_null() || builder.store_ref_bool(right));
  CHECK(!trailing_bit || builder.store_bool_bool(true));
  return builder.finalize();
}

inline td::Ref<vm::Cell> output_leaf(const block::NativeTransferRun& parent, std::size_t start, std::size_t count,
                                    td::uint32 magic = block::NativeTransferRun::outputs_leaf_magic,
                                    bool trailing_bit = false) {
  vm::CellBuilder builder;
  CHECK(builder.store_ulong_rchk_bool(magic, 32));
  CHECK(builder.store_ulong_rchk_bool(count, 5));
  for (std::size_t index = start; index < start + count; ++index) {
    const auto& output = parent.outputs[index];
    CHECK(builder.store_bits_bool(output.dst));
    CHECK(builder.store_ulong_rchk_bool(output.amount, 64));
    CHECK(builder.store_ulong_rchk_bool(output.fee, 64));
  }
  CHECK(!trailing_bit || builder.store_bool_bool(true));
  return builder.finalize();
}

inline td::Ref<vm::Cell> signature(std::size_t size = 64, bool extra_ref = false) {
  vm::CellBuilder builder;
  CHECK(builder.store_bytes_bool(std::string(size, '\x5a')));
  if (extra_ref) {
    vm::CellBuilder empty;
    CHECK(builder.store_ref_bool(empty.finalize()));
  }
  return builder.finalize();
}

inline td::Ref<vm::Cell> raw_run(const block::NativeTransferRun& parent, td::Ref<vm::Cell> signature_root,
                                td::Ref<vm::Cell> output_root, td::uint32 magic = block::NativeTransferRun::magic,
                                bool trailing_bit = false, bool trailing_ref = false) {
  vm::CellBuilder builder;
  CHECK(builder.store_ulong_rchk_bool(magic, 32));
  CHECK(builder.store_bits_bool(parent.src));
  CHECK(builder.store_ulong_rchk_bool(parent.first_nonce, 64));
  CHECK(builder.store_ulong_rchk_bool(parent.valid_until, 32));
  CHECK(builder.store_ulong_rchk_bool(parent.outputs.size(), 5));
  CHECK(signature_root.is_null() || builder.store_ref_bool(signature_root));
  CHECK(output_root.is_null() || builder.store_ref_bool(output_root));
  CHECK(!trailing_bit || builder.store_bool_bool(true));
  if (trailing_ref) {
    vm::CellBuilder empty;
    CHECK(builder.store_ref_bool(empty.finalize()));
  }
  return builder.finalize();
}

}  // namespace native_metadata_test
