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
  messages.reserve(batch.entries.size());
  if (batch.version == block::NativeTransferBatch::runs_version) {
    // `entries` is a derived execution view for v5. Its copied signature bytes
    // are not an NTFX authorization, so its synthetic external hashes must
    // never become mempool identity. Track the canonical NTRN parent once per
    // child nonce instead.
    for (const auto& run : batch.runs) {
      TRY_RESULT(parent_hash, run.external_hash());
      for (std::size_t output_index = 0; output_index < run.outputs.size(); ++output_index) {
        messages.push_back(TrackedNativeExternalMessage{
            .hash = parent_hash,
            .workchain = basechainId,
            .source = run.src,
            .nonce = run.first_nonce + static_cast<td::uint64>(output_index),
        });
      }
    }
  } else {
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
    return lhs.nonce < rhs.nonce;
  });
  messages.erase(std::unique(messages.begin(), messages.end(), [](const auto& lhs, const auto& rhs) {
                   return lhs.hash == rhs.hash && lhs.workchain == rhs.workchain && lhs.source == rhs.source &&
                          lhs.nonce == rhs.nonce;
                 }),
                 messages.end());
  return messages;
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
