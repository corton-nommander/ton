/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "ton/ton-types.h"

namespace ton::validator::consensus::simplex {

enum class NativeFinalizationRelation {
  after_canonical_top,
  canonical_top,
  canonically_decided,
};

// A canonical shard top decides every candidate at that height and below.
// The exact root at the top is canonical; a different root at the same height
// and every lower late finalization are already-decided noncanonical work.
constexpr NativeFinalizationRelation classify_native_finalization(BlockSeqno local_seqno,
                                                                  BlockSeqno canonical_seqno,
                                                                  bool is_exact_canonical_block) {
  if (local_seqno > canonical_seqno) {
    return NativeFinalizationRelation::after_canonical_top;
  }
  if (local_seqno == canonical_seqno && is_exact_canonical_block) {
    return NativeFinalizationRelation::canonical_top;
  }
  return NativeFinalizationRelation::canonically_decided;
}

}  // namespace ton::validator::consensus::simplex
