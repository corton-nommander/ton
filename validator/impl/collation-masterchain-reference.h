#pragma once

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "ton/ton-types.h"
#include "vm/cells.h"

namespace ton::validator {

// Same structural and exact identity checks as store_ext_blk_ref_to(root).
inline td::Result<td::Ref<vm::Cell>> make_collation_masterchain_reference(
    const BlockIdExt& id, td::Ref<vm::Cell> root) {
  block::gen::Block::Record rec;
  block::gen::BlockInfo::Record info;
  block::ShardId shard;
  vm::CellBuilder builder;
  if (!id.is_masterchain() || root.is_null() ||
      td::bitstring::bits_memcmp(root->get_hash().bits(), id.root_hash.bits(), 256) != 0 ||
      !tlb::unpack_cell(root, rec) || !tlb::unpack_cell(rec.info, info) ||
      !shard.deserialize(info.shard.write()) || static_cast<unsigned>(info.seq_no) != id.seqno() ||
      shard != block::ShardId{id.id} || !builder.store_long_bool(info.end_lt, 64) ||
      !builder.store_long_bool(id.seqno(), 32) || !builder.store_bits_bool(id.root_hash) ||
      !builder.store_bits_bool(id.file_hash)) {
    return td::Status::Error("invalid exact masterchain reference for collation");
  }
  return builder.finalize();
}

}  // namespace ton::validator
