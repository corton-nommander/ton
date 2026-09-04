/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <cstddef>
#include <optional>
#include <utility>

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

enum class NativeBranchRecordAction {
  retain,
  promote_and_discard,
  discard,
};

// Locally accepted branch metadata is useful only while it is ahead of the
// canonical top.  A late exact full block may satisfy a notification that won
// the accept race, but it is promoted directly and need not remain retained.
// `exact_full_match` must be false for empty candidates even when they merely
// reference the same BlockIdExt.
constexpr NativeBranchRecordAction select_native_branch_record_action(bool has_canonical_top,
                                                                       BlockSeqno local_seqno,
                                                                       BlockSeqno canonical_seqno,
                                                                       bool exact_full_match) {
  if (!has_canonical_top || local_seqno > canonical_seqno) {
    return NativeBranchRecordAction::retain;
  }
  if (local_seqno == canonical_seqno && exact_full_match) {
    return NativeBranchRecordAction::promote_and_discard;
  }
  return NativeBranchRecordAction::discard;
}

constexpr bool should_prune_native_branch_record(BlockSeqno local_seqno, BlockSeqno canonical_seqno) {
  return local_seqno <= canonical_seqno;
}

// Eight default four-slot leader windows cover ordinary finalization lag and
// competing proposals while bounding retained per-candidate nonce deltas.
// Eviction is performance-only: a broken ancestry link falls back to the
// separately proven canonical floor.
inline constexpr std::size_t native_finalized_branch_record_limit = 32;

constexpr bool native_branch_eviction_precedes(BlockSeqno lhs_seqno, bool lhs_is_full, BlockSeqno rhs_seqno,
                                                bool rhs_is_full) {
  if (lhs_seqno != rhs_seqno) {
    return lhs_seqno < rhs_seqno;
  }
  // At one height preserve full records, which alone can satisfy an exact
  // masterchain notification, ahead of empty CandidateId aliases.
  return !lhs_is_full && rhs_is_full;
}

// Reconstruct a branch-local aggregate from deltas by following one exact
// parent key at every step. Missing links (restart, pruning, bounded eviction)
// stop at the proven canonical base. The caller supplies the floor merge so
// this policy remains independent of the concrete metadata representation.
template <class BranchMap, class BranchId, class Floors, class Merge>
Floors materialize_native_branch_floor_chain(const BranchMap& exact_branches, BranchId candidate_id,
                                              Floors canonical_floors, Merge&& merge) {
  std::optional<BranchId> current = std::move(candidate_id);
  for (std::size_t depth = 0; current && depth < exact_branches.size(); ++depth) {
    auto entry = exact_branches.find(*current);
    if (entry == exact_branches.end()) {
      break;
    }
    merge(canonical_floors, entry->second.own_nonce_floors);
    current = entry->second.parent_id;
  }
  return canonical_floors;
}

template <class BranchMap, class IsProtected, class GetSeqno, class IsFull, class OnEvict>
std::size_t bound_native_branch_records(BranchMap& records, std::size_t limit, IsProtected&& is_protected,
                                        GetSeqno&& get_seqno, IsFull&& is_full, OnEvict&& on_evict) {
  std::size_t evicted = 0;
  while (records.size() > limit) {
    auto victim = records.end();
    for (auto it = records.begin(); it != records.end(); ++it) {
      if (is_protected(*it)) {
        continue;
      }
      if (victim == records.end() ||
          native_branch_eviction_precedes(get_seqno(*it), is_full(*it), get_seqno(*victim), is_full(*victim))) {
        victim = it;
      }
    }
    if (victim == records.end()) {
      break;
    }
    on_evict(*victim);
    records.erase(victim);
    ++evicted;
  }
  return evicted;
}

}  // namespace ton::validator::consensus::simplex
