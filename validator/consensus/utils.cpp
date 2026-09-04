/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "vm/boc.h"
#include "vm/cells/MerkleUpdate.h"

#include "block-auto.h"
#include "block/transaction.h"
#include "fabric.h"
#include "utils.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>

namespace ton::validator::consensus {

static_assert(native_collator_queue_max_capacity == block::NativeTransferBatch::max_entries);

std::chrono::milliseconds max_tps_candidate_timeout() {
  static const auto timeout = [] {
    constexpr long long default_ms = 5'000;
    constexpr long long min_ms = 2'000;
    constexpr long long max_ms = 60'000;
    const char* value = std::getenv("TON_SIMPLEX_MAX_TPS_CANDIDATE_TIMEOUT_MS");
    if (!value) {
      return std::chrono::milliseconds{default_ms};
    }
    errno = 0;
    char* end = nullptr;
    long long parsed = std::strtoll(value, &end, 10);
    if (errno || end == value || *end) {
      LOG(WARNING) << "Ignoring invalid TON_SIMPLEX_MAX_TPS_CANDIDATE_TIMEOUT_MS='" << value
                   << "'; using " << default_ms << "ms";
      return std::chrono::milliseconds{default_ms};
    }
    long long bounded = std::clamp(parsed, min_ms, max_ms);
    if (bounded != parsed) {
      LOG(WARNING) << "Clamping TON_SIMPLEX_MAX_TPS_CANDIDATE_TIMEOUT_MS=" << parsed << " to " << bounded
                   << "ms (valid range " << min_ms << ".." << max_ms << "ms)";
    }
    return std::chrono::milliseconds{bounded};
  }();
  return timeout;
}

std::chrono::milliseconds max_tps_candidate_work_timeout() {
  auto outer_ms = max_tps_candidate_timeout().count();
  auto eighty_percent = outer_ms * 4 / 5;
  auto with_consensus_margin = outer_ms - 1'000;
  return std::chrono::milliseconds{std::min(eighty_percent, with_consensus_margin)};
}

std::chrono::milliseconds max_tps_candidate_finalize_reserve() {
  static const auto reserve = [] {
    const auto work_budget = max_tps_candidate_work_timeout();
    auto requested = max_tps_candidate_finalize_reserve_default;
    const char* value = std::getenv("TON_SIMPLEX_MAX_TPS_FINALIZE_RESERVE_MS");
    if (value) {
      errno = 0;
      char* end = nullptr;
      long long parsed = std::strtoll(value, &end, 10);
      if (errno || end == value || *end) {
        LOG(WARNING) << "Ignoring invalid TON_SIMPLEX_MAX_TPS_FINALIZE_RESERVE_MS='" << value
                     << "'; using " << requested.count() << "ms";
      } else {
        requested = std::chrono::milliseconds{parsed};
      }
    }
    auto bounded = bound_max_tps_candidate_finalize_reserve(work_budget, requested);
    if (bounded != requested) {
      LOG(WARNING) << "Clamping TON_SIMPLEX_MAX_TPS_FINALIZE_RESERVE_MS=" << requested.count() << " to "
                   << bounded.count() << "ms for local work budget " << work_budget.count() << "ms";
    }
    return bounded;
  }();
  return reserve;
}

td::Result<double> get_candidate_gen_utime_exact(const BlockCandidate& candidate) {
  TRY_RESULT(cdata_roots, vm::std_boc_deserialize_multi(candidate.collated_data));
  for (const td::Ref<vm::Cell>& root : cdata_roots) {
    if (!block::gen::t_ConsensusExtraData.validate_ref(10000, root)) {
      continue;
    }
    block::gen::ConsensusExtraData::Record rec;
    CHECK(block::gen::unpack_cell(root, rec));
    return (double)rec.gen_utime_ms / 1000.0;
  }
  return td::Status::Error("no ConsensusExtraData in candidate");
}

td::Result<std::vector<TrackedNativeExternalMessage>> get_candidate_native_external_messages(
    const BlockCandidate& candidate) {
  std::vector<TrackedNativeExternalMessage> messages;
  if (candidate.id.is_masterchain()) {
    return messages;
  }

  TRY_RESULT(block_root, vm::std_boc_deserialize(candidate.data));
  block::gen::Block::Record block_record;
  block::gen::BlockExtra::Record extra;
  if (!tlb::unpack_cell(block_root, block_record)) {
    return td::Status::Error("cannot unpack candidate Block while extracting native external hashes");
  }
  // Some consensus tests and legacy non-native producers use an opaque,
  // reference-free extra cell. It cannot possibly carry the custom native
  // batch, so it is safe to treat it as empty without relaxing native parsing.
  if (!vm::load_cell_slice(block_record.extra).size_refs()) {
    return messages;
  }
  if (!tlb::unpack_cell(block_record.extra, extra)) {
    return td::Status::Error("cannot unpack candidate BlockExtra while extracting native external hashes");
  }
  if (!extra.custom->size_refs()) {
    return messages;
  }
  auto custom = extra.custom->prefetch_ref();
  if (custom.is_null() || vm::load_cell_slice(custom).prefetch_ulong(32) != block::NativeTransferBatch::magic) {
    return messages;
  }

  TRY_RESULT(batch, block::NativeTransferBatch::unpack(std::move(custom)));
  if (block::NativeTransferBatch::is_direct_run_version(batch.version)) {
    messages.reserve(batch.runs.size());
    // `entries` is a derived execution view for v5/v6. Its copied signature
    // bytes are not an NTFX authorization, so its synthetic external hashes
    // must never become mempool identity. Track one atomic nonce interval per
    // canonical NTRN parent instead.
    for (const auto& run : batch.runs) {
      TRY_RESULT(parent_hash, run.external_hash());
      if (run.outputs.empty() || run.outputs.size() > std::numeric_limits<td::uint32>::max()) {
        return td::Status::Error("invalid native transfer run logical count in candidate metadata");
      }
      messages.push_back(TrackedNativeExternalMessage{.hash = parent_hash,
                                                       .workchain = basechainId,
                                                       .source = run.src,
                                                       .nonce = run.first_nonce,
                                                       .logical_count = static_cast<td::uint32>(run.outputs.size())});
    }
  } else {
    messages.reserve(batch.entries.size());
    for (const auto& entry : batch.entries) {
      TRY_RESULT(hash, entry.transfer.external_hash());
      messages.push_back(TrackedNativeExternalMessage{.hash = hash,
                                                        .workchain = basechainId,
                                                        .source = entry.transfer.src,
                                                        .nonce = entry.transfer.nonce});
    }
  }
  std::sort(messages.begin(), messages.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.hash != rhs.hash) {
      return lhs.hash < rhs.hash;
    }
    if (lhs.workchain != rhs.workchain) {
      return lhs.workchain < rhs.workchain;
    }
    if (lhs.source != rhs.source) {
      return lhs.source < rhs.source;
    }
    if (lhs.nonce != rhs.nonce) {
      return lhs.nonce < rhs.nonce;
    }
    return lhs.logical_count < rhs.logical_count;
  });
  messages.erase(std::unique(messages.begin(), messages.end(), [](const auto& lhs, const auto& rhs) {
                   return lhs.hash == rhs.hash && lhs.workchain == rhs.workchain && lhs.source == rhs.source &&
                          lhs.nonce == rhs.nonce && lhs.logical_count == rhs.logical_count;
                 }),
                 messages.end());
  return messages;
}

namespace {

void normalize_native_source_nonce_floors(NativeSourceNonceFloors& floors) {
  std::sort(floors.begin(), floors.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs < rhs) {
      return true;
    }
    if (rhs < lhs) {
      return false;
    }
    return lhs.next_nonce < rhs.next_nonce;
  });

  std::size_t normalized_size = 0;
  for (std::size_t index = 0; index < floors.size(); ++index) {
    if (normalized_size != 0 && !(floors[normalized_size - 1] < floors[index]) &&
        !(floors[index] < floors[normalized_size - 1])) {
      floors[normalized_size - 1].next_nonce =
          std::max(floors[normalized_size - 1].next_nonce, floors[index].next_nonce);
    } else {
      if (normalized_size != index) {
        floors[normalized_size] = std::move(floors[index]);
      }
      ++normalized_size;
    }
  }
  floors.resize(normalized_size);
}

bool are_native_source_nonce_floors_normalized(const NativeSourceNonceFloors& floors) {
  for (std::size_t index = 1; index < floors.size(); ++index) {
    if (!(floors[index - 1] < floors[index])) {
      return false;
    }
  }
  return true;
}

bool native_source_nonce_floors_dominate(const NativeSourceNonceFloors& target,
                                         const NativeSourceNonceFloors& added) {
  auto target_it = target.begin();
  for (const auto& floor : added) {
    while (target_it != target.end() && *target_it < floor) {
      ++target_it;
    }
    if (target_it == target.end() || floor < *target_it || target_it->next_nonce < floor.next_nonce) {
      return false;
    }
  }
  return true;
}

}  // namespace

void merge_native_source_nonce_floors(NativeSourceNonceFloors& target, const NativeSourceNonceFloors& added) {
  if (added.empty()) {
    return;
  }

  // Preserve aliasing/idempotence without allocating. Although callers keep
  // `target` normalized by contract, normalize an aliased direct caller
  // defensively so merge(x, x) remains well-defined for malformed input too.
  if (&target == &added) {
    if (!are_native_source_nonce_floors_normalized(target)) {
      normalize_native_source_nonce_floors(target);
    }
    return;
  }

  // Production projections and carriers are already normalized. Keep the
  // unsorted-input API contract for direct callers, but avoid copying and
  // sorting the common case.
  NativeSourceNonceFloors normalized_storage;
  const NativeSourceNonceFloors* normalized_added = &added;
  if (!are_native_source_nonce_floors_normalized(added)) {
    normalized_storage = added;
    normalize_native_source_nonce_floors(normalized_storage);
    normalized_added = &normalized_storage;
  }

  // Recursive state resolution may reapply an unchanged canonical floor at
  // several cache boundaries. A linear dominance check leaves the target's
  // buffer untouched instead of rebuilding the full source vector each time.
  if (native_source_nonce_floors_dominate(target, *normalized_added)) {
    return;
  }
  if (target.empty()) {
    if (normalized_added == &normalized_storage) {
      target = std::move(normalized_storage);
    } else {
      target = *normalized_added;
    }
    return;
  }

  NativeSourceNonceFloors merged;
  merged.reserve(target.size() + normalized_added->size());
  auto target_it = target.begin();
  auto added_it = normalized_added->begin();
  while (target_it != target.end() && added_it != normalized_added->end()) {
    if (*target_it < *added_it) {
      merged.push_back(*target_it++);
    } else if (*added_it < *target_it) {
      merged.push_back(*added_it++);
    } else {
      auto floor = *target_it++;
      floor.next_nonce = std::max(floor.next_nonce, added_it->next_nonce);
      ++added_it;
      merged.push_back(std::move(floor));
    }
  }
  merged.insert(merged.end(), target_it, target.end());
  merged.insert(merged.end(), added_it, normalized_added->end());
  target = std::move(merged);
}

td::Result<NativeSourceNonceFloors> get_native_source_nonce_floors(
    const std::vector<TrackedNativeExternalMessage>& messages) {
  NativeSourceNonceFloors projected;
  projected.reserve(messages.size());
  for (const auto& message : messages) {
    if (message.logical_count == 0) {
      return td::Status::Error("cannot project a zero-length native nonce interval");
    }
    if (message.nonce > std::numeric_limits<td::uint64>::max() - message.logical_count) {
      return td::Status::Error("native nonce interval exclusive end overflows uint64");
    }
    projected.push_back(NativeSourceNonceFloor{.workchain = message.workchain,
                                                .source = message.source,
                                                .next_nonce = message.nonce + message.logical_count});
  }

  normalize_native_source_nonce_floors(projected);
  return projected;
}

td::Result<std::vector<Bits256>> get_candidate_native_external_hashes(const BlockCandidate& candidate) {
  TRY_RESULT(messages, get_candidate_native_external_messages(candidate));
  std::vector<Bits256> hashes;
  hashes.reserve(messages.size());
  for (const auto &message : messages) {
    hashes.push_back(message.hash);
  }
  std::sort(hashes.begin(), hashes.end());
  hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
  return hashes;
}

}  // namespace ton::validator::consensus
