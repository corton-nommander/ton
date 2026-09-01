/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "validator/fabric.h"

namespace ton::validator::consensus {

// A cancelled accept is not a locally accepted candidate.  In particular, an
// overlapping validator group may already have accepted another block at the
// same seqno, so candidate metadata must only be tracked after success.
enum class AcceptBlockAttemptDecision { applied, retry, cancelled, fatal };

constexpr AcceptBlockAttemptDecision classify_accept_block_attempt(bool succeeded, int error_code) {
  if (succeeded) {
    return AcceptBlockAttemptDecision::applied;
  }
  if (error_code == ErrorCode::timeout || error_code == ErrorCode::notready) {
    return AcceptBlockAttemptDecision::retry;
  }
  if (error_code == ErrorCode::cancelled) {
    return AcceptBlockAttemptDecision::cancelled;
  }
  return AcceptBlockAttemptDecision::fatal;
}

static_assert(classify_accept_block_attempt(true, 0) == AcceptBlockAttemptDecision::applied);
static_assert(classify_accept_block_attempt(false, ErrorCode::timeout) == AcceptBlockAttemptDecision::retry);
static_assert(classify_accept_block_attempt(false, ErrorCode::notready) == AcceptBlockAttemptDecision::retry);
static_assert(classify_accept_block_attempt(false, ErrorCode::cancelled) == AcceptBlockAttemptDecision::cancelled);

class ManagerFacade : public td::actor::Actor {
 public:
  virtual td::actor::Task<GeneratedCandidate> collate_block(CollateParams params,
                                                            td::CancellationToken cancellation_token) = 0;

  virtual td::actor::Task<ValidateCandidateResult> validate_block_candidate(BlockCandidate candidate,
                                                                            ValidateParams params,
                                                                            td::Timestamp timeout) = 0;

  virtual td::actor::Task<> accept_block(BlockIdExt id, td::Ref<BlockData> data, size_t creator_idx,
                                         td::Ref<block::BlockSignatureSet> signatures, int send_broadcast_mode,
                                         bool apply) = 0;

  // Record sources touched by a locally accepted candidate. This operation is
  // deliberately reversible: it must not advance nonce watermarks or erase
  // messages. The global pool reconciles these sources only from shard states
  // referenced by the shard-client-confirmed masterchain state.
  virtual td::actor::Task<> track_external_messages(std::vector<TrackedNativeExternalMessage> messages) {
    co_return {};
  }

  virtual td::actor::Task<td::Ref<vm::Cell>> wait_block_state_root(BlockIdExt block_id, td::Timestamp timeout) = 0;
  virtual td::actor::Task<td::Ref<BlockData>> wait_block_data(BlockIdExt block_id, td::Timestamp timeout) = 0;

  virtual void cache_block_candidate(BlockCandidate candidate) {
  }

  virtual void send_block_candidate_broadcast(BlockIdExt id, td::BufferSlice data, int mode) {
  }
};

}  // namespace ton::validator::consensus
