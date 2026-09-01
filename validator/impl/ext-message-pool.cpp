/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "ton/ton-io.hpp"

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/block.h"
#include "block/mc-config.h"
#include "td/actor/SharedFuture.h"
#include "tl/tlblib.hpp"
#include "vm/dict.h"

#include "ext-message-pool.hpp"
#include "external-message.hpp"
#include "fabric.h"
#include "transaction.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <thread>

namespace ton::validator {
void ExtMessagePool::start_up() {
  auto parse_bounded_env = [](const char* name, unsigned long fallback, unsigned long maximum) {
    const char* value = std::getenv(name);
    if (!value) {
      return fallback;
    }
    char* end = nullptr;
    auto parsed = std::strtoul(value, &end, 10);
    if (end == value || *end || parsed == 0) {
      LOG(WARNING) << "ignoring invalid " << name << "='" << value << "'";
      return fallback;
    }
    return std::min(parsed, maximum);
  };
  native_collator_queue_limit_ = parse_bounded_env("TON_NATIVE_COLLATOR_QUEUE_LIMIT", native_collator_queue_limit_,
                                                    MAX_NATIVE_COLLATOR_QUEUE_LIMIT);
  native_mempool_max_ttl_ = static_cast<td::uint32>(parse_bounded_env(
      "TON_NATIVE_MEMPOOL_MAX_TTL", native_mempool_max_ttl_, MAX_NATIVE_MEMPOOL_TTL));
  unsigned workers = 0;
  if (const char* value = std::getenv("TON_NATIVE_EXECUTOR_THREADS")) {
    char* end = nullptr;
    auto parsed = std::strtoul(value, &end, 10);
    if (end != value && !*end && parsed > 0) {
      workers = static_cast<unsigned>(std::min<unsigned long>(parsed, 64));
    }
  }
  if (!workers) {
    workers = std::clamp(std::max(1u, std::thread::hardware_concurrency()) / 2, 1u, 8u);
  }
  native_signature_verifiers_.reserve(workers);
  for (unsigned i = 0; i < workers; ++i) {
    native_signature_verifiers_.push_back(td::actor::create_actor<NativeSignatureVerifier>("native-sig-verify"));
  }
  LOG(WARNING) << "native mempool configuration: signature_workers=" << workers
               << " collator_queue_limit=" << native_collator_queue_limit_
               << " max_retention_s=" << native_mempool_max_ttl_
               << " generic_retention_s=" << MempoolMsg::GENERIC_MEMPOOL_TTL_SECONDS;
}

td::actor::Task<ExtMessagePool::CheckResult> ExtMessagePool::check_add_external_message(td::BufferSlice data,
                                                                                        int priority,
                                                                                        bool add_to_mempool) {
  co_return co_await check_add_external_message_until(std::move(data), priority, add_to_mempool,
                                                       td::Timestamp::never());
}

td::actor::Task<ExtMessagePool::CheckResult> ExtMessagePool::check_add_external_message_until(
    td::BufferSlice data, int priority, bool add_to_mempool, td::Timestamp deadline) {
  if (deadline && deadline.is_in_past()) {
    co_return td::Status::Error(ErrorCode::timeout, "external message admission deadline expired");
  }
  if (last_masterchain_state_.is_null()) {
    co_return td::Status::Error(ErrorCode::notready, "not ready");
  }
  auto r_message = create_ext_message(std::move(data), last_masterchain_state_->get_ext_msg_limits());
  if (r_message.is_error()) {
    co_return r_message.move_as_error();
  }
  co_return co_await check_add_parsed_external_message_until(r_message.move_as_ok(), priority, add_to_mempool,
                                                              deadline);
}

td::Result<td::Ref<MasterchainState>> ExtMessagePool::pin_native_admission_masterchain_state() const {
  // The manager's liteserver view is intentionally allowed to trail the applied
  // chain. Native nonce/balance admission must instead use the newest applied
  // masterchain state delivered to this actor. Holding a local Ref pins the
  // exact config/shard revision while the coroutine awaits shard-state reads.
  auto state = last_masterchain_state_;
  if (state.is_null()) {
    return td::Status::Error(ErrorCode::notready, "native admission masterchain state is not ready");
  }
  if (!state->get_block_id().is_masterchain()) {
    return td::Status::Error(ErrorCode::notready, "native admission masterchain state is invalid");
  }
  return state;
}

void ExtMessagePool::reset_native_admission_cache_generation(const BlockIdExt &masterchain_block_id) {
  if (native_admission_shard_cache_.masterchain_block_id &&
      native_admission_shard_cache_.masterchain_block_id.value() == masterchain_block_id) {
    return;
  }
  if (native_admission_shard_cache_.masterchain_block_id) {
    ++native_batch_shard_cache_generation_resets_;
  }
  native_admission_shard_cache_.masterchain_block_id = masterchain_block_id;
  native_admission_shard_cache_.shard_views.clear();
}

ExtMessagePool::NativeAdmissionShardViewPtr ExtMessagePool::lookup_native_admission_shard_view(
    const BlockIdExt &masterchain_block_id, const BlockIdExt &shard_block_id) {
  ++native_batch_shard_state_requests_;
  if (!native_admission_shard_cache_.masterchain_block_id ||
      native_admission_shard_cache_.masterchain_block_id.value() != masterchain_block_id) {
    return {};
  }
  auto it = native_admission_shard_cache_.shard_views.find(shard_block_id);
  if (it == native_admission_shard_cache_.shard_views.end()) {
    return {};
  }
  ++native_batch_shard_cache_hits_;
  return it->second;
}

td::Result<ExtMessagePool::NativeAdmissionShardViewPtr> ExtMessagePool::make_native_admission_shard_view(
    const BlockIdExt &shard_block_id, td::Ref<ShardState> state) {
  if (state.is_null()) {
    ++native_batch_shard_cache_invalid_header_;
    return td::Status::Error("native admission shard-state lookup returned a null state");
  }
  if (state->get_block_id() != shard_block_id) {
    ++native_batch_shard_cache_wrong_id_;
    return td::Status::Error("native admission shard-state lookup returned a different block");
  }
  auto root = state->root_cell();
  block::gen::ShardStateUnsplit::Record state_info;
  if (root.is_null() || !tlb::unpack_cell(root, state_info)) {
    ++native_batch_shard_cache_invalid_header_;
    return td::Status::Error("cannot unpack pinned shard state header");
  }
  block::ShardId header_shard{state_info.shard_id};
  if (!header_shard.is_valid() || ShardIdFull(header_shard) != shard_block_id.shard_full() ||
      state_info.seq_no != shard_block_id.seqno()) {
    ++native_batch_shard_cache_invalid_header_;
    return td::Status::Error("pinned shard state header identifies a different block");
  }
  RootHash state_root_hash{root->get_hash().bits()};
  if (state->root_hash() != state_root_hash || state_info.accounts.is_null()) {
    ++native_batch_shard_cache_invalid_header_;
    return td::Status::Error("pinned shard state header has an invalid state root");
  }
  NativeAdmissionShardViewPtr view = std::make_shared<const NativeAdmissionShardView>(NativeAdmissionShardView{
      .block_id = shard_block_id,
      .state_root_hash = state_root_hash,
      .gen_utime = state_info.gen_utime,
      .gen_lt = state_info.gen_lt,
      .accounts = std::move(state_info.accounts),
  });
  return view;
}

td::Result<ExtMessagePool::NativeAdmissionShardViewPtr> ExtMessagePool::store_native_admission_shard_view(
    const BlockIdExt &masterchain_block_id, NativeAdmissionShardViewPtr view) {
  if (!view) {
    return td::Status::Error("cannot cache a null native admission shard view");
  }
  // A batch may resume after a newer applied masterchain notification cleared
  // the cache. Its exact pinned state remains valid for that batch, but it must
  // never repopulate the newer generation with an old topology.
  if (!native_admission_shard_cache_.masterchain_block_id ||
      native_admission_shard_cache_.masterchain_block_id.value() != masterchain_block_id) {
    ++native_batch_shard_cache_stale_generation_fill_skips_;
    return view;
  }
  auto [it, inserted] = native_admission_shard_cache_.shard_views.emplace(view->block_id, view);
  if (!inserted) {
    ++native_batch_shard_cache_fill_races_;
    if (it->second->state_root_hash != view->state_root_hash) {
      ++native_batch_shard_cache_fill_conflicts_;
      return td::Status::Error("conflicting native admission shard-state cache fill for the same block");
    }
    return it->second;
  }
  ++native_batch_shard_cache_fills_;
  native_batch_shard_cache_peak_entries_ =
      std::max<td::uint64>(native_batch_shard_cache_peak_entries_, native_admission_shard_cache_.shard_views.size());
  return view;
}

void ExtMessagePool::record_native_admission_manager_wait_error(const td::Status &error) {
  ++native_batch_shard_miss_errors_;
  ++native_batch_shard_manager_wait_errors_;
  if (error.code() == ErrorCode::timeout || error.code() == td::actor::AWAIT_TIMEOUT_CODE) {
    ++native_batch_shard_manager_wait_timeouts_;
  } else if (error.code() == ErrorCode::notready) {
    ++native_batch_shard_manager_wait_notready_;
  } else {
    ++native_batch_shard_manager_wait_other_errors_;
  }
}

bool ExtMessagePool::native_admission_manager_wait_finished_after_deadline(td::Timestamp deadline) {
  if (!deadline || !deadline.is_in_past()) {
    return false;
  }
  ++native_batch_shard_manager_wait_late_results_;
  return true;
}

td::Result<td::optional<ExtMessagePool::CheckResult>> ExtMessagePool::check_existing_external_message(
    td::Ref<ExtMessage> message, int priority, bool add_to_mempool) {
  if (add_to_mempool) {
    auto existing = ext_messages_hashes_.find(message->hash());
    if (existing != ext_messages_hashes_.end()) {
      auto [existing_priority, existing_id] = existing->second;
      auto priority_it = ext_msgs_.find(existing_priority);
      CHECK(priority_it != ext_msgs_.end());
      auto existing_message = priority_it->second.ext_messages_.find(existing_id);
      CHECK(existing_message);
      bool stale_native_admission = false;
      if (existing_message.value()->native_nonce) {
        auto address = existing_message.value()->address();
        auto account_it = native_accounts_.find(address);
        auto watermark_it = native_nonce_watermarks_.find(address);
        if (account_it == native_accounts_.end() || watermark_it == native_nonce_watermarks_.end()) {
          stale_native_admission = true;
        } else {
          auto reservation_it = account_it->second.messages.find(existing_message.value()->native_nonce.value());
          stale_native_admission =
              reservation_it == account_it->second.messages.end() ||
              (reservation_it != account_it->second.messages.end() &&
               (reservation_it->second.hash != existing_message.value()->message->hash() ||
                watermark_it->second.is_consumed(existing_message.value()->native_nonce.value()) ||
                (!reservation_it->second.committed &&
                 reservation_it->second.account_revision != watermark_it->second.revision)));
          if (!stale_native_admission && reservation_it->second.committed &&
              reservation_it->second.account_revision != watermark_it->second.revision) {
            // A canonical account advance changes the watermark revision for
            // every still-pending higher nonce.  A late byte-identical retry
            // must not erase that committed head before the fresh admission
            // check has a chance to finish: if the check then times out, every
            // later nonce from the source is stranded. Canonical
            // reconciliation validates balance and rebases the affordable
            // surviving prefix atomically.
            ++native_exact_retry_preserved_stale_revision_;
          }
        }
      }
      if (existing_message.value()->expired() || stale_native_admission) {
        // Cleanup is intentionally infrequent, but raw-hash idempotence must
        // never turn an expired or balance-stale, uncollatable entry into an
        // apparent success.
        CHECK(erase_message(existing_priority, existing_id));
        CHECK(ext_messages_hashes_.find(message->hash()) == ext_messages_hashes_.end());
        existing = ext_messages_hashes_.end();
      }
    }
    if (existing != ext_messages_hashes_.end() && existing->second.first >= priority) {
      // sendMessage is idempotent for a byte-identical message that is still in
      // the mempool.  In particular, a client can safely retry after losing the
      // first response without repeating the expensive account/signature check.
      auto [wait_allow_broadcast, allow_broadcast_promise] = td::actor::StartedTask<>::make_bridge();
      allow_broadcast_promise.set_value(td::Unit{});
      return td::optional<CheckResult>{CheckResult{.message = std::move(message),
                                                   .wait_allow_broadcast = std::move(wait_allow_broadcast),
                                                   .should_broadcast = false,
                                                   .msg_seqno = {},
                                                   .native_transfer = {}}};
    }
  }
  return td::optional<CheckResult>{};
}

td::Result<ExtMessagePool::CheckResult> ExtMessagePool::finalize_checked_message(CheckResult result, int priority,
                                                                                  bool add_to_mempool,
                                                                                  td::Timestamp deadline) {
  if (!result.should_broadcast) {
    return std::move(result);
  }
  auto message = result.message;
  auto native_transfer = result.native_transfer ? &result.native_transfer.value() : nullptr;
  auto wc = message->wc();
  auto addr = message->addr();
  ++total_check_ext_messages_ok_;
  if (deadline && deadline.is_in_past()) {
    rollback_checked_message(message, result.msg_seqno, native_transfer);
    return td::Status::Error(ErrorCode::timeout, "external message admission deadline expired");
  }
  if (checked_ext_msg_counter_.inc_msg_count(wc, addr) > MAX_EXT_MSG_PER_ADDR) {
    rollback_checked_message(message, result.msg_seqno, native_transfer);
    return td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex());
  }
  if (add_to_mempool) {
    auto add_status = add_message_to_mempool(message, priority, result.msg_seqno, native_transfer);
    if (add_status.is_error()) {
      rollback_checked_message(message, result.msg_seqno, native_transfer);
      return std::move(add_status);
    }
  }
  auto commit_status = commit_checked_message(message, result.msg_seqno, native_transfer);
  if (commit_status.is_error()) {
    if (add_to_mempool) {
      erase_message(priority, MessageId{message->shard(), message->hash()});
    }
    rollback_checked_message(message, result.msg_seqno, native_transfer);
    return std::move(commit_status);
  }
  if (add_to_mempool && native_transfer != nullptr) {
    // Native reservations become executable only after commit_message().  A
    // wake during insertion observes committed=false and can strand the first
    // (or final) head until unrelated ingress arrives.
    std::set<NativeAddress> committed_source{{wc, addr}};
    // If this source already has a valid ready head, this commit can only make
    // a later nonce available. Keep that token instead of accumulating a stale
    // replacement on every high-rate ingress event.
    wake_native_callbacks(&committed_source, true);
  }
  return std::move(result);
}

td::actor::Task<ExtMessagePool::CheckResult> ExtMessagePool::check_add_parsed_external_message_until(
    td::Ref<ExtMessage> message, int priority, bool add_to_mempool, td::Timestamp deadline) {
  if (deadline && deadline.is_in_past()) {
    co_return td::Status::Error(ErrorCode::timeout, "external message admission deadline expired");
  }
  auto r_existing = check_existing_external_message(message, priority, add_to_mempool);
  if (r_existing.is_error()) {
    co_return r_existing.move_as_error();
  }
  auto existing = r_existing.move_as_ok();
  if (existing) {
    co_return std::move(existing.value());
  }
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  if (checked_ext_msg_counter_.get_msg_count(wc, addr) >= MAX_EXT_MSG_PER_ADDR) {
    co_return td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex());
  }
  auto result = co_await check_message(message, deadline).wrap();
  if (result.is_error()) {
    ++total_check_ext_messages_error_;
    co_return result.move_as_error();
  }
  auto finalized = finalize_checked_message(result.move_as_ok(), priority, add_to_mempool, deadline);
  if (finalized.is_error()) {
    ++total_check_ext_messages_error_;
    co_return finalized.move_as_error();
  }
  co_return finalized.move_as_ok();
}

td::actor::Task<ExtMessagePool::BatchCheckResult> ExtMessagePool::check_add_external_messages_until(
    std::vector<td::BufferSlice> batch, int priority, bool add_to_mempool, td::Timestamp deadline) {
  BatchCheckResult output;
  output.statuses.resize(batch.size());
  std::vector<bool> status_set(batch.size(), false);
  auto reject = [&](std::size_t index, td::Status error) {
    output.statuses[index] = ExternalMessageAdmissionResult::failure(std::move(error));
    status_set[index] = true;
  };
  auto accept = [&](std::size_t index) {
    output.statuses[index] = ExternalMessageAdmissionResult::success();
    status_set[index] = true;
  };

  ++native_batch_count_;
  native_batch_messages_ += batch.size();
  if (deadline && deadline.is_in_past()) {
    for (std::size_t i = 0; i < batch.size(); ++i) {
      reject(i, td::Status::Error(ErrorCode::timeout, "external message admission deadline expired"));
    }
    native_batch_rejected_ += batch.size();
    log_native_batch_stats();
    co_return output;
  }
  if (last_masterchain_state_.is_null()) {
    for (std::size_t i = 0; i < batch.size(); ++i) {
      reject(i, td::Status::Error(ErrorCode::notready, "not ready"));
    }
    native_batch_rejected_ += batch.size();
    log_native_batch_stats();
    co_return output;
  }

  struct BatchItem {
    td::Ref<ExtMessage> message;
    td::optional<block::NativeTransfer> native_transfer;
    td::optional<std::size_t> duplicate_of;
  };
  std::vector<BatchItem> items(batch.size());
  std::map<ExtMessage::Hash, std::size_t> first_by_hash;
  std::vector<std::size_t> generic_indices;
  std::vector<std::size_t> native_indices;
  auto limits = last_masterchain_state_->get_ext_msg_limits();
  for (std::size_t i = 0; i < batch.size(); ++i) {
    if (deadline && deadline.is_in_past()) {
      reject(i, td::Status::Error(ErrorCode::timeout, "external message admission deadline expired"));
      continue;
    }
    auto r_message = create_ext_message(std::move(batch[i]), limits);
    if (r_message.is_error()) {
      reject(i, r_message.move_as_error());
      continue;
    }
    items[i].message = r_message.move_as_ok();
    auto [it, inserted] = first_by_hash.emplace(items[i].message->hash(), i);
    if (!inserted) {
      items[i].duplicate_of = it->second;
      continue;
    }
    ++native_batch_unique_messages_;
    auto native_transfer = block::NativeTransfer::unpack_external(items[i].message->root_cell());
    if (native_transfer.is_ok()) {
      items[i].native_transfer = native_transfer.move_as_ok();
      native_indices.push_back(i);
    } else {
      generic_indices.push_back(i);
    }
  }

  // Generic externals retain the full TVM/wallet fallback. They still benefit
  // from batch-level parsing/deduplication, while native transfers below use a
  // single pinned chain revision and no per-item LiteQuery actor.
  std::vector<td::actor::StartedTask<CheckResult>> generic_tasks;
  generic_tasks.reserve(generic_indices.size());
  for (auto index : generic_indices) {
    generic_tasks.push_back(
        check_add_parsed_external_message_until(items[index].message, priority, add_to_mempool, deadline).start());
  }
  if (!generic_tasks.empty()) {
    auto generic_results = co_await td::actor::all_wrap(std::move(generic_tasks));
    for (std::size_t i = 0; i < generic_results.size(); ++i) {
      auto index = generic_indices[i];
      if (generic_results[i].is_error()) {
        reject(index, generic_results[i].move_as_error());
        continue;
      }
      auto checked = generic_results[i].move_as_ok();
      if (checked.should_broadcast) {
        output.checked_messages.push_back(std::move(checked));
      }
      accept(index);
    }
  }

  std::map<NativeAddress, std::vector<std::size_t>> source_items;
  for (auto index : native_indices) {
    auto &message = items[index].message;
    const auto &transfer = items[index].native_transfer.value();
    auto existing = check_existing_external_message(message, priority, add_to_mempool);
    if (existing.is_error()) {
      reject(index, existing.move_as_error());
      continue;
    }
    auto existing_result = existing.move_as_ok();
    if (existing_result) {
      accept(index);
      continue;
    }
    if (checked_ext_msg_counter_.get_msg_count(message->wc(), message->addr()) >= MAX_EXT_MSG_PER_ADDR) {
      reject(index, td::Status::Error(PSTRING() << "too many external messages to address " << message->wc() << ":"
                                                << message->addr().to_hex()));
      continue;
    }
    if (message->wc() != basechainId || transfer.src != message->addr()) {
      reject(index, td::Status::Error("native transfer is routed to the wrong source account"));
      continue;
    }
    if (transfer.valid_until <= static_cast<UnixTime>(td::Clocks::system())) {
      reject(index, td::Status::Error("native transfer valid_until is in the past"));
      continue;
    }
    source_items[{message->wc(), message->addr()}].push_back(index);
  }

  struct SourceSnapshot {
    UnixTime utime{0};
    LogicalTime lt{0};
    td::uint64 balance{0};
    td::uint64 first_nonce{0};
    td::uint64 revision{0};
  };
  std::map<NativeAddress, SourceSnapshot> source_snapshots;
  std::set<NativeAddress> changed_native_sources;
  Bits256 chain_domain;
  if (!source_items.empty()) {
    auto mc_result = pin_native_admission_masterchain_state();
    if (mc_result.is_error()) {
      auto error = mc_result.move_as_error();
      for (const auto &[_, indices] : source_items) {
        for (auto index : indices) {
          reject(index, td::Status::Error(error.code(), error.message().str()));
        }
      }
      source_items.clear();
    } else {
      auto mc_state = mc_result.move_as_ok();
      auto mc_block_id = mc_state->get_block_id();
      // Keep the cache generation tied to the exact state pinned by this
      // actor turn. update_last_masterchain_state normally established it,
      // while this idempotent reset also makes the invariant local to the
      // admission path (including tests and future initialization paths).
      reset_native_admission_cache_generation(mc_block_id);
      ++native_batch_mc_state_pins_;
      native_batch_last_pinned_mc_seqno_ = mc_block_id.seqno();
      auto config_result =
          block::ConfigInfo::extract_config(mc_state->root_cell(), mc_block_id, 0xFFFF);
      if (config_result.is_error()) {
        auto error = config_result.move_as_error();
        for (const auto &[_, indices] : source_items) {
          for (auto index : indices) {
            reject(index, td::Status::Error(error.code(), error.message().str()));
          }
        }
        source_items.clear();
      } else {
        chain_domain = config_result.ok()->get_zerostate_id().root_hash;
        std::map<BlockIdExt, std::vector<NativeAddress>> shard_sources;
        for (const auto &[address, indices] : source_items) {
          auto shard = mc_state->get_shard_from_config(extract_addr_prefix(address.first, address.second).as_leaf_shard(),
                                                       false);
          if (shard.is_null()) {
            for (auto index : indices) {
              reject(index, td::Status::Error(ErrorCode::notready,
                                              "cannot locate native source shard in pinned masterchain state"));
            }
            continue;
          }
          shard_sources[shard->top_block_id()].push_back(address);
        }

        for (const auto &[shard_block_id, addresses] : shard_sources) {
          auto reject_shard = [&](td::Status error) {
            auto code = error.code();
            auto message = error.message().str();
            for (const auto &address : addresses) {
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error(code, message));
              }
            }
          };
          if (deadline && deadline.is_in_past()) {
            reject_shard(td::Status::Error(ErrorCode::timeout, "external message admission deadline expired"));
            continue;
          }
          auto shard_view = lookup_native_admission_shard_view(mc_block_id, shard_block_id);
          if (!shard_view) {
            ++native_batch_shard_manager_waits_;
            auto state_result = co_await td::actor::await_with_timeout(
                                    td::actor::ask(manager_, &ValidatorManager::wait_block_state_short,
                                                   shard_block_id, 0, deadline, false),
                                    deadline)
                                    .wrap();
            if (state_result.is_error()) {
              auto error = state_result.move_as_error();
              record_native_admission_manager_wait_error(error);
              reject_shard(std::move(error));
              continue;
            }
            // The state read can win the timeout race at the deadline. Stop
            // before validating, caching, or changing canonical watermarks.
            if (native_admission_manager_wait_finished_after_deadline(deadline)) {
              reject_shard(td::Status::Error(ErrorCode::timeout, "external message admission deadline expired"));
              continue;
            }
            auto view_result = make_native_admission_shard_view(shard_block_id, state_result.move_as_ok());
            if (view_result.is_error()) {
              ++native_batch_shard_miss_errors_;
              reject_shard(view_result.move_as_error());
              continue;
            }
            auto stored_view = store_native_admission_shard_view(mc_block_id, view_result.move_as_ok());
            if (stored_view.is_error()) {
              ++native_batch_shard_miss_errors_;
              reject_shard(stored_view.move_as_error());
              continue;
            }
            shard_view = stored_view.move_as_ok();
          }
          native_batch_last_pinned_shard_seqno_ = shard_view->block_id.seqno();
          if (mc_state->get_unix_time() >= shard_view->gen_utime) {
            native_batch_max_mc_shard_utime_lag_s_ = std::max<td::uint64>(
                native_batch_max_mc_shard_utime_lag_s_, mc_state->get_unix_time() - shard_view->gen_utime);
          }
          vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(shard_view->accounts), 256,
                                           block::tlb::aug_ShardAccounts};
          for (const auto &address : addresses) {
            ++native_batch_account_lookups_;
            block::Account account;
            auto shard_account = accounts.lookup(address.second);
            if (!account.unpack(shard_account, shard_view->gen_utime, false)) {
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error("Failed to unpack account state"));
              }
              continue;
            }
            account.block_lt = shard_view->gen_lt;
            if (account.status != block::Account::acc_uninit || !account.is_native) {
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error("native transfer source account must be balance-only"));
              }
              continue;
            }
            auto available_balance = account.native_balance_uint64();
            if (!available_balance) {
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error(
                                  "native transfer source balance must be uint64 grams without extra currencies"));
              }
              continue;
            }
            auto applied = apply_canonical_native_account_state(address, account.native_nonce,
                                                                 available_balance.value(), shard_view->gen_utime,
                                                                 shard_view->gen_lt);
            if (applied.is_error()) {
              ++native_batch_watermark_lag_rejections_;
              const auto &watermark = native_nonce_watermarks_.at(address);
              if (watermark.observed_next_nonce > account.native_nonce) {
                native_batch_max_watermark_nonce_lag_ =
                    std::max(native_batch_max_watermark_nonce_lag_,
                             watermark.observed_next_nonce - account.native_nonce);
              }
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error(
                                  ErrorCode::notready,
                                  "native account state predates the latest observed canonical state"));
              }
              continue;
            }
            if (applied.ok()) {
              changed_native_sources.insert(address);
            }
            const auto &watermark = native_nonce_watermarks_.at(address);
            auto first_nonce = watermark.first_unconsumed_nonce();
            if (!first_nonce) {
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error("native account nonce space is exhausted"));
              }
              continue;
            }
            source_snapshots[address] = SourceSnapshot{.utime = shard_view->gen_utime,
                                                       .lt = shard_view->gen_lt,
                                                       .balance = available_balance.value(),
                                                       .first_nonce = first_nonce.value(),
                                                       .revision = watermark.revision};
          }
        }
      }
    }
  }
  if (!changed_native_sources.empty()) {
    wake_native_callbacks(&changed_native_sources);
  }

  std::vector<std::size_t> verify_indices;
  std::vector<td::actor::StartedTask<td::Unit>> verification_tasks;
  for (const auto &[address, indices] : source_items) {
    auto snapshot_it = source_snapshots.find(address);
    if (snapshot_it == source_snapshots.end()) {
      continue;
    }
    const auto &snapshot = snapshot_it->second;
    for (auto index : indices) {
      if (status_set[index]) {
        continue;
      }
      const auto &transfer = items[index].native_transfer.value();
      if (transfer.nonce < snapshot.first_nonce) {
        reject(index, td::Status::Error(PSTRING() << "Too old native nonce: msg_nonce=" << transfer.nonce
                                                  << ", account_nonce=" << snapshot.first_nonce));
        continue;
      }
      if (transfer.nonce - snapshot.first_nonce > MAX_NATIVE_NONCE_DIFF) {
        reject(index, td::Status::Error(PSTRING() << "Too new native nonce: msg_nonce=" << transfer.nonce
                                                  << ", account_nonce=" << snapshot.first_nonce));
        continue;
      }
      auto required_amount = transfer.amount + transfer.fee;
      if (required_amount < transfer.amount) {
        reject(index, td::Status::Error("native transfer amount and fee overflow"));
        continue;
      }
      if (required_amount > snapshot.balance) {
        reject(index, td::Status::Error("native transfer has insufficient source balance"));
        continue;
      }
      CHECK(!native_signature_verifiers_.empty());
      auto &verifier =
          native_signature_verifiers_[native_signature_verifier_cursor_++ % native_signature_verifiers_.size()];
      verification_tasks.push_back(
          td::actor::await_with_timeout(td::actor::ask(verifier, &NativeSignatureVerifier::verify, transfer,
                                                       chain_domain),
                                        deadline)
              .start());
      verify_indices.push_back(index);
    }
  }
  if (!verification_tasks.empty()) {
    auto verification_results = co_await td::actor::all_wrap(std::move(verification_tasks));
    for (std::size_t i = 0; i < verification_results.size(); ++i) {
      if (verification_results[i].is_error()) {
        reject(verify_indices[i], verification_results[i].move_as_error());
      }
    }
  }

  std::vector<NativeAdmissionOrderKey> order_keys;
  for (auto index : verify_indices) {
    if (!status_set[index]) {
      const auto &message = items[index].message;
      order_keys.push_back(NativeAdmissionOrderKey{.workchain = message->wc(),
                                                   .source = message->addr(),
                                                   .nonce = items[index].native_transfer.value().nonce,
                                                   .hash = message->hash(),
                                                   .input_index = index});
    }
  }
  for (auto index : order_native_admissions(std::move(order_keys))) {
    auto address = NativeAddress{items[index].message->wc(), items[index].message->addr()};
    const auto &snapshot = source_snapshots.at(address);
    auto reserved = co_await reserve_verified_native_message(items[index].message,
                                                              items[index].native_transfer.value(), snapshot.balance,
                                                              snapshot.revision, snapshot.utime, deadline)
                        .wrap();
    if (reserved.is_error()) {
      ++total_check_ext_messages_error_;
      reject(index, reserved.move_as_error());
      continue;
    }
    auto finalized = finalize_checked_message(reserved.move_as_ok(), priority, add_to_mempool, deadline);
    if (finalized.is_error()) {
      ++total_check_ext_messages_error_;
      reject(index, finalized.move_as_error());
      continue;
    }
    auto checked = finalized.move_as_ok();
    if (checked.should_broadcast) {
      output.checked_messages.push_back(std::move(checked));
    }
    accept(index);
  }

  for (std::size_t i = 0; i < items.size(); ++i) {
    if (items[i].duplicate_of) {
      auto primary = items[i].duplicate_of.value();
      if (!status_set[primary]) {
        reject(primary, td::Status::Error("batch admission did not produce a primary result"));
      }
      output.statuses[i] = output.statuses[primary];
      status_set[i] = true;
    }
    if (!status_set[i]) {
      reject(i, td::Status::Error("batch admission did not produce a result"));
    }
  }
  for (const auto &status : output.statuses) {
    if (status.accepted) {
      ++native_batch_accepted_;
    } else {
      ++native_batch_rejected_;
    }
  }
  log_native_batch_stats();
  co_return output;
}

void ExtMessagePool::log_native_batch_stats() {
  if (!native_batch_log_at_.is_in_past()) {
    return;
  }
  LOG(INFO) << "external-message batch admission cumulative: batches=" << native_batch_count_
            << " messages=" << native_batch_messages_ << " unique=" << native_batch_unique_messages_
            << " shard_state_requests=" << native_batch_shard_state_requests_
            << " shard_manager_waits=" << native_batch_shard_manager_waits_
            << " shard_fetches=" << native_batch_shard_manager_waits_
            << " shard_cache_hits=" << native_batch_shard_cache_hits_
            << " shard_cache_fills=" << native_batch_shard_cache_fills_
            << " shard_cache_fill_races=" << native_batch_shard_cache_fill_races_
            << " shard_cache_fill_conflicts=" << native_batch_shard_cache_fill_conflicts_
            << " shard_cache_generation_resets=" << native_batch_shard_cache_generation_resets_
            << " shard_cache_stale_generation_fill_skips="
            << native_batch_shard_cache_stale_generation_fill_skips_
            << " shard_cache_wrong_id=" << native_batch_shard_cache_wrong_id_
            << " shard_cache_invalid_header=" << native_batch_shard_cache_invalid_header_
            << " shard_miss_errors=" << native_batch_shard_miss_errors_
            << " shard_fetch_errors=" << native_batch_shard_miss_errors_
            << " shard_manager_wait_errors=" << native_batch_shard_manager_wait_errors_
            << " shard_manager_wait_timeouts=" << native_batch_shard_manager_wait_timeouts_
            << " shard_manager_wait_notready=" << native_batch_shard_manager_wait_notready_
            << " shard_manager_wait_other_errors=" << native_batch_shard_manager_wait_other_errors_
            << " shard_manager_wait_late_results=" << native_batch_shard_manager_wait_late_results_
            << " shard_cache_entries=" << native_admission_shard_cache_.shard_views.size()
            << " shard_cache_peak_entries=" << native_batch_shard_cache_peak_entries_
            << " account_lookups=" << native_batch_account_lookups_
            << " accepted=" << native_batch_accepted_ << " rejected=" << native_batch_rejected_
            << " mc_state_pins=" << native_batch_mc_state_pins_
            << " pinned_mc_seqno=" << native_batch_last_pinned_mc_seqno_
            << " pinned_shard_seqno=" << native_batch_last_pinned_shard_seqno_
            << " max_mc_shard_utime_lag_s=" << native_batch_max_mc_shard_utime_lag_s_
            << " watermark_lag_rejections=" << native_batch_watermark_lag_rejections_
            << " max_watermark_nonce_lag=" << native_batch_max_watermark_nonce_lag_
            << " ignored_mc_state_updates=" << native_batch_ignored_mc_state_updates_
            << " reconciliation_tracked_candidates=" << native_reconciliation_tracked_candidates_
            << " reconciliation_tracked_messages=" << native_reconciliation_tracked_messages_
            << " reconciliation_runs=" << native_reconciliation_runs_
            << " reconciliation_sources_advanced=" << native_reconciliation_sources_advanced_
            << " reconciliation_messages_purged=" << native_reconciliation_messages_purged_
            << " reconciliation_failures=" << native_reconciliation_failures_
            << " reconciliation_unchanged_state_skips=" << native_reconciliation_unchanged_state_skips_
            << " reconciliation_unchanged_top_skips=" << native_reconciliation_unchanged_top_skips_
            << " reconciliation_unchanged_source_skips=" << native_reconciliation_unchanged_source_skips_
            << " reconciliation_rebased_reservations=" << native_reconciliation_rebased_reservations_
            << " reconciliation_unaffordable_tail_pruned="
            << native_reconciliation_unaffordable_tail_pruned_
            << " reconciliation_stale_uncommitted_tail_pruned="
            << native_reconciliation_stale_uncommitted_tail_pruned_
            << " expiry_suffix_events=" << native_expiry_suffix_events_
            << " expiry_suffix_pruned=" << native_expiry_suffix_pruned_
            << " exact_retry_preserved_stale_revision=" << native_exact_retry_preserved_stale_revision_;
  native_batch_log_at_ = td::Timestamp::in(1.0);
}

ExtMessagePool::NativeQueueSelection ExtMessagePool::select_native_messages(
    ShardIdFull shard, const std::vector<ExtMessage::Hash> &excluded_messages,
    const std::set<ExtMessage::Hash> &already_delivered, std::size_t limit,
    td::optional<NativeAddress> cursor, const std::set<NativeAddress> *source_filter) {
  NativeQueueSelection selection;
  selection.cursor = cursor;
  if (shard.workchain == masterchainId || limit == 0) {
    return selection;
  }

  struct SourceState {
    NativeAddress source;
    const NativeInfo *info{nullptr};
    td::uint64 canonical_nonce{0};
    td::uint64 next_nonce{0};
    td::optional<NativeQueueItem> next;
    bool blocked{false};
    bool ready_counted{false};
  };
  std::vector<SourceState> sources;
  sources.reserve(source_filter ? source_filter->size() : native_accounts_.size());
  auto add_source = [&](const NativeAddress &source, const NativeInfo &info) {
    if (!shard_contains(shard, extract_addr_prefix(source.first, source.second))) {
      return;
    }
    auto watermark_it = native_nonce_watermarks_.find(source);
    if (watermark_it == native_nonce_watermarks_.end()) {
      if (!info.messages.empty()) {
        ++selection.counters.head_gaps;
        ++selection.counters.head_missing_watermark;
      }
      return;
    }
    auto first_nonce = watermark_it->second.first_unconsumed_nonce();
    if (!first_nonce) {
      return;
    }
    sources.push_back(SourceState{.source = source,
                                  .info = &info,
                                  .canonical_nonce = first_nonce.value(),
                                  .next_nonce = first_nonce.value(),
                                  .next = {},
                                  .blocked = false,
                                  .ready_counted = false});
  };
  if (source_filter) {
    for (const auto &source : *source_filter) {
      auto info = native_accounts_.find(source);
      if (info != native_accounts_.end()) {
        add_source(info->first, info->second);
      }
    }
  } else {
    for (const auto &[source, info] : native_accounts_) {
      add_source(source, info);
    }
  }

  // Address order is stable, but each queue install begins immediately after
  // the source used last time. A bounded candidate therefore cannot starve the
  // tail of a large source set merely because its address sorts late.
  auto after_cursor = [&](const NativeAddress &source) {
    return !cursor || source > cursor.value();
  };
  std::sort(sources.begin(), sources.end(), [&](const SourceState &lhs, const SourceState &rhs) {
    bool lhs_after = after_cursor(lhs.source);
    bool rhs_after = after_cursor(rhs.source);
    if (lhs_after != rhs_after) {
      return lhs_after;
    }
    return lhs.source < rhs.source;
  });

  auto advance_nonce = [](SourceState &source) {
    if (source.next_nonce == std::numeric_limits<td::uint64>::max()) {
      source.blocked = true;
      return false;
    }
    ++source.next_nonce;
    return true;
  };
  auto probe = [&](SourceState &source) {
    source.next = {};
    while (!source.blocked) {
      auto reservation_it = source.info->messages.lower_bound(source.next_nonce);
      if (reservation_it == source.info->messages.end()) {
        if (source.next_nonce > source.canonical_nonce) {
          ++selection.counters.speculative_exhausted;
        } else {
          ++selection.counters.head_gaps;
          ++selection.counters.head_missing_nonce;
        }
        source.blocked = true;
        return;
      }
      if (reservation_it->first != source.next_nonce) {
        ++selection.counters.head_gaps;
        ++selection.counters.head_missing_nonce;
        source.blocked = true;
        return;
      }
      if (!reservation_it->second.committed) {
        ++selection.counters.head_gaps;
        ++selection.counters.head_uncommitted;
        source.blocked = true;
        return;
      }
      ++selection.counters.scanned;
      const auto &hash = reservation_it->second.hash;
      auto pool_it = ext_messages_hashes_.find(hash);
      if (pool_it == ext_messages_hashes_.end()) {
        ++selection.counters.head_gaps;
        ++selection.counters.head_missing_hash_index;
        source.blocked = true;
        return;
      }
      auto priority_it = ext_msgs_.find(pool_it->second.first);
      if (priority_it == ext_msgs_.end()) {
        ++selection.counters.head_gaps;
        ++selection.counters.head_missing_priority;
        source.blocked = true;
        return;
      }
      auto message = priority_it->second.ext_messages_.find(pool_it->second.second);
      if (!message) {
        ++selection.counters.head_gaps;
        ++selection.counters.head_missing_message;
        source.blocked = true;
        return;
      }
      if (!message.value()->native_nonce || message.value()->native_nonce.value() != source.next_nonce) {
        ++selection.counters.head_gaps;
        ++selection.counters.head_nonce_mismatch;
        source.blocked = true;
        return;
      }

      // Exclusions describe transfers already applied by the speculative
      // parent. They advance only this callback's view; the canonical watermark
      // and pool entry remain untouched so a losing fork can offer them again.
      if (std::binary_search(excluded_messages.begin(), excluded_messages.end(), hash)) {
        ++selection.counters.excluded;
        if (!advance_nonce(source)) {
          return;
        }
        continue;
      }
      if (already_delivered.contains(hash)) {
        ++selection.counters.already_delivered;
        if (!advance_nonce(source)) {
          return;
        }
        continue;
      }
      auto &mempool_message = *message.value();
      if (mempool_message.expired()) {
        ++selection.counters.expired;
        source.blocked = true;
        return;
      }
      bool was_active = mempool_message.active;
      if (!mempool_message.is_active()) {
        ++selection.counters.inactive;
        selection.earliest_reactivation.relax(mempool_message.reactivate_at);
        source.blocked = true;
        return;
      }
      if (!was_active) {
        ++selection.counters.reactivated;
      }
      ++selection.counters.active;
      if (!source.ready_counted) {
        source.ready_counted = true;
        ++selection.counters.ready_sources;
      }
      source.next = NativeQueueItem{.message = mempool_message.message,
                                    .priority = pool_it->second.first,
                                    .source = source.source,
                                    .nonce = source.next_nonce};
      return;
    }
  };

  std::map<int, std::deque<std::size_t>> ready_by_priority;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    probe(sources[i]);
    if (sources[i].next) {
      ready_by_priority[sources[i].next.value().priority].push_back(i);
    }
  }
  while (selection.items.size() < limit && !ready_by_priority.empty()) {
    int priority = ready_by_priority.rbegin()->first;
    auto &ready = ready_by_priority[priority];
    auto source_index = ready.front();
    ready.pop_front();
    if (ready.empty()) {
      ready_by_priority.erase(priority);
    }
    auto &source = sources[source_index];
    std::size_t run_size = 0;
    while (selection.items.size() < limit && run_size < NATIVE_SOURCE_RUN_TARGET && source.next &&
           source.next.value().priority == priority) {
      selection.items.push_back(std::move(source.next.value()));
      source.next = {};
      ++selection.counters.selected;
      ++run_size;
      selection.cursor = source.source;
      if (!advance_nonce(source)) {
        break;
      }
      probe(source);
    }
    if (run_size != 0) {
      ++selection.counters.runs;
      selection.counters.run_messages += run_size;
      selection.counters.max_run_size = std::max<td::uint64>(selection.counters.max_run_size, run_size);
    }
    if (source.next) {
      ready_by_priority[source.next.value().priority].push_back(source_index);
    }
  }
  return selection;
}

void ExtMessagePool::enqueue_callback_native_source(CallbackNativeScheduler &scheduler,
                                                    const NativeAddress &source,
                                                    CallbackNativeSource &state) {
  if (!state.next || state.queued) {
    return;
  }
  scheduler.ready_by_priority[state.next.value().priority].push_back(
      CallbackNativeReadyToken{source, state.generation});
  state.queued = true;
}

void ExtMessagePool::probe_callback_native_source(const std::shared_ptr<InstalledCallback> &callback,
                                                  const NativeAddress &source, CallbackNativeSource &state,
                                                  NativeQueueCounters &counters, bool enqueue_ready) {
  ++counters.source_probes;
  ++state.generation;
  state.next = {};
  state.queued = false;

  auto info_it = native_accounts_.find(source);
  auto watermark_it = native_nonce_watermarks_.find(source);
  if (info_it == native_accounts_.end()) {
    return;
  }
  if (watermark_it == native_nonce_watermarks_.end()) {
    if (!info_it->second.messages.empty()) {
      ++counters.head_gaps;
      ++counters.head_missing_watermark;
    }
    return;
  }
  auto first_nonce = watermark_it->second.first_unconsumed_nonce();
  if (!first_nonce) {
    return;
  }
  state.next_nonce = std::max(state.next_nonce, first_nonce.value());
  auto advance_nonce = [&] {
    if (state.next_nonce == std::numeric_limits<td::uint64>::max()) {
      return false;
    }
    ++state.next_nonce;
    return true;
  };

  while (true) {
    auto reservation_it = info_it->second.messages.lower_bound(state.next_nonce);
    if (reservation_it == info_it->second.messages.end()) {
      if (state.next_nonce > first_nonce.value()) {
        ++counters.speculative_exhausted;
      } else {
        ++counters.head_gaps;
        ++counters.head_missing_nonce;
      }
      return;
    }
    if (reservation_it->first != state.next_nonce) {
      ++counters.head_gaps;
      ++counters.head_missing_nonce;
      return;
    }
    if (!reservation_it->second.committed) {
      ++counters.head_gaps;
      ++counters.head_uncommitted;
      return;
    }
    ++counters.scanned;
    const auto &hash = reservation_it->second.hash;
    auto pool_it = ext_messages_hashes_.find(hash);
    if (pool_it == ext_messages_hashes_.end()) {
      ++counters.head_gaps;
      ++counters.head_missing_hash_index;
      return;
    }
    auto priority_it = ext_msgs_.find(pool_it->second.first);
    if (priority_it == ext_msgs_.end()) {
      ++counters.head_gaps;
      ++counters.head_missing_priority;
      return;
    }
    auto message = priority_it->second.ext_messages_.find(pool_it->second.second);
    if (!message) {
      ++counters.head_gaps;
      ++counters.head_missing_message;
      return;
    }
    if (!message.value()->native_nonce || message.value()->native_nonce.value() != state.next_nonce) {
      ++counters.head_gaps;
      ++counters.head_nonce_mismatch;
      return;
    }
    if (std::binary_search(callback->callback->excluded_messages.begin(),
                           callback->callback->excluded_messages.end(), hash)) {
      ++counters.excluded;
      if (!advance_nonce()) {
        return;
      }
      continue;
    }
    if (callback->delivered_native.contains(hash)) {
      ++counters.already_delivered;
      if (!advance_nonce()) {
        return;
      }
      continue;
    }
    auto &mempool_message = *message.value();
    if (mempool_message.expired()) {
      ++counters.expired;
      return;
    }
    bool was_active = mempool_message.active;
    if (!mempool_message.is_active()) {
      ++counters.inactive;
      alarm_timestamp().relax(mempool_message.reactivate_at);
      return;
    }
    if (!was_active) {
      ++counters.reactivated;
    }
    ++counters.active;
    if (!state.ready_counted) {
      state.ready_counted = true;
      ++counters.ready_sources;
    }
    state.next = NativeQueueItem{.message = mempool_message.message,
                                 .priority = pool_it->second.first,
                                 .source = source,
                                 .nonce = state.next_nonce};
    if (enqueue_ready) {
      enqueue_callback_native_source(callback->native_scheduler, source, state);
    }
    return;
  }
}

void ExtMessagePool::refresh_callback_native_source(const std::shared_ptr<InstalledCallback> &callback,
                                                    const NativeAddress &source, NativeQueueCounters &counters,
                                                    bool count_refresh) {
  if (count_refresh) {
    ++counters.source_refreshes;
  }
  auto &scheduler = callback->native_scheduler;
  if (!shard_contains(callback->callback->shard, extract_addr_prefix(source.first, source.second))) {
    scheduler.sources.erase(source);
    return;
  }
  auto info_it = native_accounts_.find(source);
  auto watermark_it = native_nonce_watermarks_.find(source);
  if (info_it == native_accounts_.end() || watermark_it == native_nonce_watermarks_.end()) {
    scheduler.sources.erase(source);
    return;
  }
  auto first_nonce = watermark_it->second.first_unconsumed_nonce();
  if (!first_nonce) {
    scheduler.sources.erase(source);
    return;
  }
  auto [state_it, inserted] = scheduler.sources.try_emplace(source);
  if (inserted) {
    state_it->second.next_nonce = first_nonce.value();
  } else {
    state_it->second.next_nonce = std::max(state_it->second.next_nonce, first_nonce.value());
  }
  probe_callback_native_source(callback, source, state_it->second, counters, true);
}

void ExtMessagePool::initialize_callback_native_scheduler(const std::shared_ptr<InstalledCallback> &callback,
                                                          NativeQueueCounters &counters) {
  auto &scheduler = callback->native_scheduler;
  if (scheduler.initialized) {
    return;
  }
  scheduler.initialized = true;
  ++counters.scheduler_builds;
  if (callback->callback->shard.workchain == masterchainId) {
    return;
  }

  auto split = callback->native_cursor ? native_accounts_.upper_bound(callback->native_cursor.value())
                                       : native_accounts_.begin();
  auto scan_range = [&](auto begin, auto end) {
    for (auto it = begin; it != end; ++it) {
      ++counters.source_scans;
      refresh_callback_native_source(callback, it->first, counters, false);
    }
  };
  scan_range(split, native_accounts_.end());
  scan_range(native_accounts_.begin(), split);
}

ExtMessagePool::NativeQueueSelection ExtMessagePool::select_callback_native_messages(
    const std::shared_ptr<InstalledCallback> &callback, std::size_t limit,
    const std::set<NativeAddress> *source_filter) {
  NativeQueueSelection selection;
  selection.cursor = callback->native_cursor;
  if (callback->callback->shard.workchain == masterchainId || limit == 0) {
    return selection;
  }
  auto &scheduler = callback->native_scheduler;
  bool initialized = scheduler.initialized;
  initialize_callback_native_scheduler(callback, selection.counters);
  if (initialized && source_filter) {
    for (const auto &source : *source_filter) {
      refresh_callback_native_source(callback, source, selection.counters, true);
    }
  }

  while (selection.items.size() < limit && !scheduler.ready_by_priority.empty()) {
    auto priority_it = std::prev(scheduler.ready_by_priority.end());
    int priority = priority_it->first;
    auto token = priority_it->second.front();
    priority_it->second.pop_front();
    if (priority_it->second.empty()) {
      scheduler.ready_by_priority.erase(priority_it);
    }
    auto source_it = scheduler.sources.find(token.source);
    if (source_it == scheduler.sources.end() || source_it->second.generation != token.generation ||
        !source_it->second.queued || !source_it->second.next ||
        source_it->second.next.value().priority != priority) {
      ++selection.counters.stale_ready_tokens;
      continue;
    }
    auto &state = source_it->second;
    state.queued = false;

    // Pool mutation can occur while the producer is suspended on queue
    // backpressure. Revalidate only this ready source before consuming its run;
    // no map reference survives an await.
    probe_callback_native_source(callback, token.source, state, selection.counters, false);
    if (!state.next) {
      continue;
    }
    if (state.next.value().priority != priority) {
      enqueue_callback_native_source(scheduler, token.source, state);
      continue;
    }

    std::size_t run_size = 0;
    while (selection.items.size() < limit && run_size < NATIVE_SOURCE_RUN_TARGET && state.next &&
           state.next.value().priority == priority) {
      selection.items.push_back(std::move(state.next.value()));
      state.next = {};
      ++selection.counters.selected;
      ++run_size;
      selection.cursor = token.source;
      if (state.next_nonce == std::numeric_limits<td::uint64>::max()) {
        break;
      }
      ++state.next_nonce;
      probe_callback_native_source(callback, token.source, state, selection.counters, false);
    }
    if (run_size != 0) {
      ++selection.counters.runs;
      selection.counters.run_messages += run_size;
      selection.counters.max_run_size =
          std::max<td::uint64>(selection.counters.max_run_size, run_size);
    }
    enqueue_callback_native_source(scheduler, token.source, state);
  }
  return selection;
}

void ExtMessagePool::enqueue_callback_item(const std::shared_ptr<InstalledCallback> &callback,
                                           std::pair<td::Ref<ExtMessage>, int> item, bool native) {
  auto& pending = native ? callback->pending_native : callback->pending_generic;
  pending.push_back(ExtMsgQueueEntry::make_message(std::move(item), native));
}

void ExtMessagePool::begin_callback_epoch(const std::shared_ptr<InstalledCallback> &callback) {
  if (!callback->callback->native_streaming) {
    return;
  }
  if (!callback->producer_epoch_open) {
    callback->producer_epoch = callback->callback->queue_state->begin_producer_epoch();
    callback->producer_epoch_open = true;
  }
  callback->native_snapshot_exhausted = false;
}

void ExtMessagePool::start_callback_pump(const std::shared_ptr<InstalledCallback> &callback,
                                         CallbackPumpStart start) {
  if (!callback->pump_active) {
    callback->pump_active = true;
    auto task = pump_callback(callback);
    if (start == CallbackPumpStart::initial_native_fast_lane) {
      CHECK(callback->callback->native_streaming);
      // The callback was just installed through the guarded Collator ->
      // Manager -> Pool fast lane. Start only its first producer turn while
      // this pool actor is still idle, so the queued native prefix reaches the
      // BackpressureQueue without another pool mailbox turn. The coroutine
      // suspends on the existing deferred queue ask; live wakes and all later
      // refills continue through the normal deferred start below.
      std::move(task).start_immediate().detach();
    } else {
      std::move(task).start().detach();
    }
  }
}

void ExtMessagePool::cancel_callback_delivery(const std::shared_ptr<InstalledCallback> &callback) {
  callback->callback->queue_state->record_cancel_discarded();
  callback->pending_native.clear();
  callback->pending_generic.clear();
  callback->callback->queue.close();
}

td::actor::Task<> ExtMessagePool::pump_callback(std::shared_ptr<InstalledCallback> callback) {
  while (callback->callback->cancellation_token.check().is_ok()) {
    if (callback->native_scheduler_rebuild) {
      callback->native_scheduler = {};
      callback->native_dirty_sources.clear();
      callback->native_scheduler_rebuild = false;
    } else if (!callback->native_dirty_sources.empty()) {
      NativeQueueCounters counters;
      if (callback->native_scheduler.initialized) {
        // No scheduler map reference crosses the queue await below. Repeated
        // ingress for one source is coalesced by native_dirty_sources.
        for (const auto &source : callback->native_dirty_sources) {
          refresh_callback_native_source(callback, source, counters, true);
        }
      }
      callback->native_dirty_sources.clear();
      native_queue_counters_.add(counters);
    }
    if (callback->pending_native.empty() && callback->callback->native_streaming &&
        !callback->native_snapshot_exhausted && native_transport_has_refill_credit(*callback)) {
      callback->native_snapshot_exhausted = fill_callback_native(callback, false) == 0;
    }
    auto* pending = !callback->pending_native.empty() ? &callback->pending_native : &callback->pending_generic;
    if (!pending->empty()) {
      auto batch_capacity = NATIVE_DELIVERY_CHUNK;
      if (pending == &callback->pending_native && callback->callback->native_streaming) {
        // The initial native prefill is a whole bounded transport window, so
        // publish it in one FIFO request. Later refills remain one 512-item
        // scheduler fragment and block behind that window as needed.
        batch_capacity = std::min(callback->callback->transport_message_capacity, pending->size());
      }
      std::vector<ExtMsgQueueEntry> batch;
      std::vector<bool> native_entries;
      batch.reserve(batch_capacity);
      native_entries.reserve(batch_capacity);
      while (batch.size() < batch_capacity && !pending->empty()) {
        native_entries.push_back(pending->front().native);
        batch.push_back(std::move(pending->front()));
        pending->pop_front();
      }
      auto batch_size = batch.size();
      std::size_t native_reserved = 0;
      for (bool native : native_entries) {
        native_reserved += native;
      }
      // Publish ownership of the whole native batch before the blocking push.
      // BackpressureQueue exposes inserted prefixes to a blocked consumer as
      // space becomes available, before this coroutine resumes with the final
      // prefix count. The reservation is therefore the consumer's safe bound.
      callback->callback->queue_state->record_push_started(native_reserved);
      std::size_t pushed;
      if (callback->callback->native_streaming) {
        pushed = co_await callback->callback->queue.push_many_bounded(
            std::move(batch), callback->callback->transport_message_capacity);
      } else {
        pushed = co_await callback->callback->queue.push_many(std::move(batch));
      }
      // The pump only batches message entries. Count the native prefix that
      // actually entered the queue if cancellation closed it mid-batch.
      std::size_t native_pushed = 0;
      for (std::size_t i = 0; i < pushed; ++i) {
        native_pushed += native_entries[i];
      }
      callback->callback->queue_state->record_push_completed(native_reserved, native_pushed);
      if (pushed != batch_size) {
        // push_many returns a short prefix only when the queue was closed.
        cancel_callback_delivery(callback);
        break;
      }
      continue;
    }
    if (callback->callback->native_streaming) {
      auto epoch = callback->producer_epoch;
      if (callback->completion_epoch < epoch) {
        // Close this epoch before awaiting the marker push. If ingress arrives
        // while suspended, it opens a newer epoch whose messages necessarily
        // follow this marker in the same FIFO queue.
        callback->producer_epoch_open = false;
        if (!co_await callback->callback->queue.push(ExtMsgQueueEntry::make_completion(epoch))) {
          cancel_callback_delivery(callback);
          break;
        }
        callback->completion_epoch = epoch;
        // Work may have arrived while the completion marker was blocked behind
        // a full transport window. A newer epoch is therefore handled next.
        continue;
      }
    }
    break;
  }
  if (callback->callback->cancellation_token.check().is_error()) {
    cancel_callback_delivery(callback);
  }
  callback->pump_active = false;
  if (callback->callback->sync_only) {
    callback->callback->queue.close();
  }
  co_return {};
}

std::size_t ExtMessagePool::fill_callback_native(const std::shared_ptr<InstalledCallback> &callback,
                                                 bool count_install,
                                                 const std::set<NativeAddress> *source_filter,
                                                 std::size_t max_items) {
  NativeQueueCounters counters;
  if (count_install) {
    counters.installs = 1;
  }
  if (callback->callback->shard.workchain == masterchainId) {
    counters.masterchain_installs = count_install ? 1 : 0;
    native_queue_counters_.add(counters);
    return 0;
  }
  auto delivery_limit = std::min(native_collator_queue_limit_, callback->callback->queue_capacity);
  if (callback->delivered_native.size() >= delivery_limit) {
    native_queue_counters_.add(counters);
    return 0;
  }
  auto remaining = std::min({delivery_limit - callback->delivered_native.size(), NATIVE_DELIVERY_CHUNK, max_items});
  if (remaining == 0) {
    native_queue_counters_.add(counters);
    return 0;
  }
  auto selection = select_callback_native_messages(callback, remaining, source_filter);
  selection.counters.add(counters);
  callback->native_cursor = selection.cursor;
  if (selection.earliest_reactivation) {
    alarm_timestamp().relax(selection.earliest_reactivation);
  }
  auto selected = selection.items.size();
  for (auto &item : selection.items) {
    callback->delivered_native.insert(item.message->hash());
    enqueue_callback_item(callback, {std::move(item.message), item.priority}, true);
  }
  callback->callback->queue_state->record_selected(selected);
  native_queue_counters_.add(selection.counters);
  return selected;
}

std::size_t ExtMessagePool::prefill_callback_native(const std::shared_ptr<InstalledCallback> &callback,
                                                    bool count_install) {
  if (!callback->callback->native_streaming) {
    return fill_callback_native(callback, count_install);
  }
  // Seed the queue with the complete bounded transport window before starting
  // its producer coroutine. Selection itself remains 512-message fair chunks;
  // only the initial producer hand-off is widened.
  const auto delivery_limit = std::min(native_collator_queue_limit_, callback->callback->queue_capacity);
  const auto prefill_limit = std::min(delivery_limit, callback->callback->transport_message_capacity);
  std::size_t selected = 0;
  bool count_this_fill = count_install;
  while (callback->pending_native.size() < prefill_limit) {
    auto remaining = prefill_limit - callback->pending_native.size();
    auto filled = fill_callback_native(callback, count_this_fill, nullptr, remaining);
    count_this_fill = false;
    selected += filled;
    if (filled == 0) {
      callback->native_snapshot_exhausted = true;
      break;
    }
  }
  return selected;
}

std::size_t ExtMessagePool::native_transport_selected_limit(const InstalledCallback &callback) const {
  // The physical queue holds one configured window. The producer may stage or
  // reserve one additional 512-message fragment while waiting for space, but
  // never another full candidate snapshot.
  return callback.callback->transport_message_capacity + NATIVE_DELIVERY_CHUNK;
}

bool ExtMessagePool::native_transport_has_refill_credit(const InstalledCallback &callback) const {
  return callback.callback->queue_state->native_selected_ahead() < native_transport_selected_limit(callback);
}

std::size_t ExtMessagePool::wake_native_callbacks(const std::set<NativeAddress> *source_filter,
                                                  bool preserve_valid_ready_head) {
  std::size_t woken = 0;
  std::erase_if(callbacks_, [&](const std::shared_ptr<InstalledCallback> &callback) {
    if (callback->callback->cancellation_token.check().is_error() ||
        (callback->callback->timeout && callback->callback->timeout.is_in_past())) {
      cancel_callback_delivery(callback);
      return true;
    }
    bool marked = false;
    if (source_filter) {
      for (const auto &source : *source_filter) {
        if (preserve_valid_ready_head && callback->native_scheduler.initialized) {
          auto state_it = callback->native_scheduler.sources.find(source);
          if (state_it != callback->native_scheduler.sources.end() && state_it->second.queued &&
              state_it->second.next) {
            continue;
          }
        }
        marked |= callback->native_dirty_sources.insert(source).second;
      }
    } else if (!callback->native_scheduler_rebuild) {
      callback->native_scheduler_rebuild = true;
      callback->native_dirty_sources.clear();
      marked = true;
    }
    if (marked) {
      begin_callback_epoch(callback);
      ++woken;
    }
    // Selection is demand-driven inside the serialized pump. In particular,
    // a live ingress wake cannot append another 512 items while the previous
    // batch is blocked behind a full transport window.
    start_callback_pump(callback);
    return false;
  });
  return woken;
}

std::size_t ExtMessagePool::reactivate_due_native_messages(td::Timestamp now) {
  std::size_t reactivated = 0;
  std::set<NativeAddress> reactivated_sources;
  while (!native_reactivations_.empty() && native_reactivations_.begin()->first.is_in_past(now)) {
    auto scheduled = native_reactivations_.begin()->second;
    native_reactivations_.erase(native_reactivations_.begin());
    auto priority_it = ext_msgs_.find(scheduled.first);
    if (priority_it == ext_msgs_.end()) {
      continue;
    }
    auto message = priority_it->second.ext_messages_.find(scheduled.second);
    if (!message || !message.value()->native_nonce || message.value()->expired() || message.value()->active) {
      continue;
    }
    if (!message.value()->reactivate_at.is_in_past(now)) {
      native_reactivations_.emplace(message.value()->reactivate_at, scheduled);
      continue;
    }
    if (message.value()->is_active()) {
      ++reactivated;
      reactivated_sources.insert(message.value()->address());
    }
  }
  if (reactivated != 0) {
    native_queue_counters_.reactivated += reactivated;
    native_queue_counters_.reactivation_wakes += wake_native_callbacks(&reactivated_sources);
  }
  return reactivated;
}

void ExtMessagePool::install_collator_queue(ShardIdFull shard, std::unique_ptr<ExtMsgCallback> callback) {
  std::sort(callback->excluded_messages.begin(), callback->excluded_messages.end());
  callback->excluded_messages.erase(
      std::unique(callback->excluded_messages.begin(), callback->excluded_messages.end()),
      callback->excluded_messages.end());
  auto installed = std::make_shared<InstalledCallback>(std::move(callback));
  if (installed->callback->native_streaming) {
    installed->callback->queue_state->attach_telemetry(native_transport_telemetry_);
    begin_callback_epoch(installed);
  }

  // Native scheduling is deliberately skipped for masterchain installs. Native
  // transfers are basechain-only, and copying/scanning their large treaps while
  // producing a masterchain anchor created avoidable multi-second stalls.
  installed->native_cursor = native_scheduler_cursor_;
  prefill_callback_native(installed, true);
  if (installed->callback->shard.workchain != masterchainId) {
    native_scheduler_cursor_ = installed->native_cursor;
  }

  // Generic externals retain their stable priority/address order. Masterchain
  // installs are bounded to the normal queue capacity even in max-TPS mode.
  // This pool-side bound prevents a large basechain native backlog from
  // influencing anchor production without changing generic admission rules.
  shard = installed->callback->shard;
  td::uint64 lo_prefix = shard.shard & (shard.shard - 1);
  td::uint64 hi_prefix_plus1 = (shard.shard | (shard.shard - 1)) + 1;  // may overflow to 0
  MessageId shard_lo{AccountIdPrefixFull{shard.workchain, lo_prefix}, Bits256::zero()};
  MessageId shard_hi{AccountIdPrefixFull{hi_prefix_plus1 == 0 ? shard.workchain + 1 : shard.workchain, hi_prefix_plus1},
                     Bits256::zero()};
  auto generic_limit = shard.workchain == masterchainId ? STANDARD_COLLATOR_QUEUE_LIMIT
                                                        : std::numeric_limits<std::size_t>::max();
  if (installed->callback->sync_only) {
    auto remaining_capacity = installed->callback->queue_capacity > installed->delivered_native.size()
                                  ? installed->callback->queue_capacity - installed->delivered_native.size()
                                  : 0;
    generic_limit = std::min(generic_limit, remaining_capacity);
  }
  for (auto it = ext_msgs_.rbegin(); it != ext_msgs_.rend(); ++it) {
    auto [_, in_shard, __] = it->second.generic_messages_.split_range(shard_lo, shard_hi);
    auto iterator = in_shard.in_order();
    while (installed->generic_selected < generic_limit) {
      auto item = iterator.next();
      if (!item) {
        break;
      }
      auto [key, msg] = std::move(item.value());
      if (msg->expired() || !msg->is_active() ||
          std::binary_search(installed->callback->excluded_messages.begin(),
                             installed->callback->excluded_messages.end(), msg->message->hash())) {
        continue;
      }
      ++installed->generic_selected;
      enqueue_callback_item(installed, {msg->message, it->first}, false);
    }
    if (installed->generic_selected >= generic_limit) {
      break;
    }
  }

  VLOG(VALIDATOR_DEBUG) << "install_collator_queue: selected_native=" << installed->delivered_native.size()
                        << " selected_generic=" << installed->generic_selected
                        << " excluded=" << installed->callback->excluded_messages.size()
                        << " native_limit=" << native_collator_queue_limit_ << " shard=" << shard;
  if (!installed->callback->sync_only) {
    alarm_timestamp().relax(installed->callback->timeout);
    // The initial native producer may run in this actor turn. Register the
    // callback first so a concurrent ingress wake always observes the same
    // live callback that owns the queue reservation.
    callbacks_.push_back(installed);
  }
  start_callback_pump(installed, installed->callback->native_streaming ? CallbackPumpStart::initial_native_fast_lane
                                                                         : CallbackPumpStart::deferred);
}

void ExtMessagePool::cleanup_external_messages(ShardIdFull shard) {
  // Clean up expired messages
  for (auto &[priority, msgs] : ext_msgs_) {
    std::vector<MessageId> to_erase;
    auto iterator = msgs.ext_messages_.in_order();
    while (auto item = iterator.next()) {
      auto [key, msg] = std::move(item.value());
      if (shard_contains(shard, key.dst) && msg->expired()) {
        to_erase.push_back(key);
      }
    }
    for (auto &id : to_erase) {
      erase_message(priority, id);
    }
  }
}

void ExtMessagePool::complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                                std::vector<ExtMessage::Hash> to_delete) {
  for (auto &hash : to_delete) {
    auto it = ext_messages_hashes_.find(hash);
    if (it != ext_messages_hashes_.end()) {
      erase_message(it->second.first, it->second.second);
    }
  }
  for (auto &hash : to_delay) {
    auto it = ext_messages_hashes_.find(hash);
    if (it != ext_messages_hashes_.end()) {
      int priority = it->second.first;
      auto msg_id = it->second.second;
      auto &msgs = ext_msgs_[priority];
      auto msg_opt = msgs.ext_messages_.find(msg_id);
      if (msg_opt && msg_opt.value()->native_nonce && !msg_opt.value()->expired()) {
        // A native message may be delayed simply because the current candidate is
        // full, timed out, or lost consensus.  Never evict it for retry count or
        // soft-pool pressure: deleting one nonce permanently blocks later nonces.
        if (msg_opt.value()->postpone_native()) {
          ++native_queue_counters_.delayed;
          native_reactivations_.emplace(msg_opt.value()->reactivate_at, std::make_pair(priority, msg_id));
          alarm_timestamp().relax(msg_opt.value()->reactivate_at);
        }
      } else if (msg_opt && msgs.ext_messages_.size() < SOFT_MEMPOOL_LIMIT && msg_opt.value()->can_postpone()) {
        msg_opt.value()->postpone();
      } else {
        erase_message(priority, msg_id);
      }
    }
  }
}

void ExtMessagePool::track_locally_accepted_native_messages(
    std::vector<TrackedNativeExternalMessage> messages) {
  if (messages.empty()) {
    return;
  }
  ++native_reconciliation_tracked_candidates_;
  native_reconciliation_tracked_messages_ += messages.size();
  for (const auto &message : messages) {
    NativeAddress address{message.workchain, message.source};
    // A candidate learned from another node need not have a corresponding
    // local reservation. There is nothing to purge in that case, and keeping a
    // source-only hint forever would turn losing candidates into a scan leak.
    auto account_it = native_accounts_.find(address);
    if (account_it == native_accounts_.end() || account_it->second.messages.empty()) {
      continue;
    }
    auto [it, inserted] = locally_accepted_native_nonces_.emplace(address, message.nonce);
    if (!inserted) {
      it->second = std::max(it->second, message.nonce);
    }
  }
  // Do not rescan every pending source against an unchanged applied state for
  // every locally accepted candidate.  At state arrival all then-pending
  // reservations are registered below. A reservation inserted afterwards was
  // admitted against an equally new or newer canonical watermark, so the next
  // applied state is sufficient.
}

void ExtMessagePool::reconcile_native_external_messages(td::Ref<MasterchainState> state) {
  if (state.is_null() || !state->get_block_id().is_masterchain()) {
    ++native_reconciliation_failures_;
    return;
  }
  const auto block_id = state->get_block_id();
  if (applied_reconciliation_state_.not_null()) {
    const auto current_id = applied_reconciliation_state_->get_block_id();
    if (block_id.seqno() < current_id.seqno() ||
        (block_id.seqno() == current_id.seqno() && block_id != current_id)) {
      ++native_reconciliation_failures_;
      LOG(WARNING) << "Ignoring non-monotonic applied masterchain state for native reconciliation: current="
                   << current_id << " update=" << block_id;
      return;
    }
  }
  auto fingerprint = native_reconciliation_state_fingerprint(state);
  applied_reconciliation_state_ = state;
  native_reconciliation_last_mc_seqno_ = block_id.seqno();
  // Masterchain blocks often advance without changing any referenced basechain
  // shard top. Once a complete walk of this exact topology succeeded, neither
  // regrouping every source nor refetching an immutable state can discover new
  // canonical account progress. New reservations are also safe to defer: they
  // were admitted against this state or a newer one, so this old top cannot
  // already contain them. Failed walks never publish the fingerprint.
  if (should_skip_native_reconciliation_state(fingerprint)) {
    return;
  }
  // Correctness must not depend on a successful local BlockAccepter callback:
  // a canonical block can be learned through shard-client sync, an observer,
  // or an overlapping validator group. Reconcile every source for which this
  // pool can actually have stale native reservations.
  register_pending_native_reconciliation_targets();
  ++native_reconciliation_generation_;
  start_native_reconciliation();
}

void ExtMessagePool::register_pending_native_reconciliation_targets() {
  for (auto it = locally_accepted_native_nonces_.begin(); it != locally_accepted_native_nonces_.end();) {
    auto account_it = native_accounts_.find(it->first);
    if (account_it == native_accounts_.end() || account_it->second.messages.empty()) {
      it = locally_accepted_native_nonces_.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto &[address, account] : native_accounts_) {
    if (account.messages.empty()) {
      continue;
    }
    const auto max_pending_nonce = account.messages.rbegin()->first;
    auto [it, inserted] = locally_accepted_native_nonces_.emplace(address, max_pending_nonce);
    if (!inserted) {
      it->second = std::max(it->second, max_pending_nonce);
    }
  }
}

void ExtMessagePool::prune_native_reconciliation_target_if_idle(const NativeAddress &address) {
  auto account_it = native_accounts_.find(address);
  if (account_it == native_accounts_.end() || account_it->second.messages.empty()) {
    locally_accepted_native_nonces_.erase(address);
  }
}

void ExtMessagePool::start_native_reconciliation() {
  if (native_reconciliation_active_ || applied_reconciliation_state_.is_null() ||
      locally_accepted_native_nonces_.empty()) {
    return;
  }
  native_reconciliation_active_ = true;
  run_native_reconciliation().start().detach();
}

td::actor::Task<> ExtMessagePool::run_native_reconciliation() {
  while (applied_reconciliation_state_.not_null() && !locally_accepted_native_nonces_.empty()) {
    const auto generation = native_reconciliation_generation_;
    auto state = applied_reconciliation_state_;
    std::vector<NativeAddress> sources;
    sources.reserve(locally_accepted_native_nonces_.size());
    for (const auto &[source, _] : locally_accepted_native_nonces_) {
      sources.push_back(source);
    }
    ++native_reconciliation_runs_;
    auto result = co_await reconcile_native_snapshot(std::move(state), std::move(sources)).wrap();
    if (result.is_error()) {
      ++native_reconciliation_failures_;
      LOG(WARNING) << "Native applied-state reconciliation was incomplete: " << result.error();
    }
    if (generation == native_reconciliation_generation_) {
      break;
    }
  }
  native_reconciliation_active_ = false;
  co_return {};
}

td::actor::Task<> ExtMessagePool::reconcile_native_snapshot(td::Ref<MasterchainState> state,
                                                             std::vector<NativeAddress> sources) {
  std::map<BlockIdExt, std::vector<NativeAddress>> shard_sources;
  td::Status first_error;
  for (const auto &address : sources) {
    auto shard = state->get_shard_from_config(extract_addr_prefix(address.first, address.second).as_leaf_shard(),
                                              false);
    if (shard.is_null()) {
      if (first_error.is_ok()) {
        first_error = td::Status::Error(ErrorCode::notready,
                                        "cannot locate tracked native source in applied shard configuration");
      }
      continue;
    }
    shard_sources[shard->top_block_id()].push_back(address);
  }

  std::set<NativeAddress> changed_sources;
  for (const auto &[shard_block_id, addresses] : shard_sources) {
    // Masterchain blocks commonly keep referencing the same shard top while
    // that shard is busy. The referenced state is immutable, so rescanning
    // every pending native source cannot discover new canonical progress and
    // can starve the shard that would produce the next top. A source tracked
    // after this top was scanned is also safe to defer: the old top cannot
    // contain a message accepted after it. Failed/incomplete scans are never
    // recorded and therefore retry on the next applied masterchain state.
    if (!should_reconcile_native_shard_top(shard_block_id, addresses.size())) {
      continue;
    }
    bool shard_complete = true;
    ++native_reconciliation_state_fetches_;
    auto state_result =
        co_await td::actor::await_with_timeout(
                     td::actor::ask(manager_, &ValidatorManager::get_block_state_for_litequery, shard_block_id),
                     td::Timestamp::in(30.0))
            .wrap();
    if (state_result.is_error()) {
      shard_complete = false;
      if (first_error.is_ok()) {
        first_error = state_result.move_as_error_prefix("cannot load applied shard state: ");
      }
      continue;
    }
    auto shard_state = state_result.move_as_ok();
    if (shard_state->get_block_id() != shard_block_id) {
      shard_complete = false;
      if (first_error.is_ok()) {
        first_error = td::Status::Error("applied shard-state lookup returned a different block");
      }
      continue;
    }
    native_reconciliation_last_shard_seqno_ =
        std::max<td::uint64>(native_reconciliation_last_shard_seqno_, shard_state->get_seqno());
    block::gen::ShardStateUnsplit::Record state_info;
    if (!tlb::unpack_cell(shard_state->root_cell(), state_info)) {
      shard_complete = false;
      if (first_error.is_ok()) {
        first_error = td::Status::Error("cannot unpack applied shard state for native reconciliation");
      }
      continue;
    }
    vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(state_info.accounts), 256,
                                     block::tlb::aug_ShardAccounts};
    for (const auto &address : addresses) {
      ++native_reconciliation_account_lookups_;
      block::Account account;
      auto shard_account = accounts.lookup(address.second);
      if (!account.unpack(shard_account, state_info.gen_utime, false)) {
        shard_complete = false;
        if (first_error.is_ok()) {
          first_error = td::Status::Error("cannot unpack tracked native account from applied shard state");
        }
        continue;
      }
      account.block_lt = state_info.gen_lt;
      if (account.status != block::Account::acc_uninit || !account.is_native) {
        shard_complete = false;
        if (first_error.is_ok()) {
          first_error = td::Status::Error("tracked native source is no longer a balance-only account");
        }
        continue;
      }
      auto balance = account.native_balance_uint64();
      if (!balance) {
        shard_complete = false;
        if (first_error.is_ok()) {
          first_error = td::Status::Error("tracked native source balance is not uint64 grams");
        }
        continue;
      }
      auto applied = apply_canonical_native_account_state(address, account.native_nonce, balance.value(),
                                                          state_info.gen_utime, state_info.gen_lt);
      if (applied.is_error()) {
        shard_complete = false;
        if (first_error.is_ok()) {
          first_error = applied.move_as_error_prefix("cannot apply canonical native account state: ");
        }
        continue;
      }
      if (applied.ok()) {
        changed_sources.insert(address);
      }
    }
    if (shard_complete) {
      record_successful_native_shard_reconciliation(shard_block_id);
    }
  }
  if (!changed_sources.empty()) {
    wake_native_callbacks(&changed_sources);
  }
  if (first_error.is_error()) {
    co_return std::move(first_error);
  }
  record_successful_native_reconciliation_state(native_reconciliation_state_fingerprint(state));
  co_return {};
}

bool ExtMessagePool::should_reconcile_native_shard_top(const BlockIdExt &shard_block_id,
                                                        std::size_t source_count) {
  auto it = native_reconciliation_successful_shard_tops_.find(shard_block_id.shard_full());
  if (it == native_reconciliation_successful_shard_tops_.end() || it->second != shard_block_id) {
    return true;
  }
  ++native_reconciliation_unchanged_top_skips_;
  native_reconciliation_unchanged_source_skips_ += source_count;
  return false;
}

void ExtMessagePool::record_successful_native_shard_reconciliation(const BlockIdExt &shard_block_id) {
  native_reconciliation_successful_shard_tops_[shard_block_id.shard_full()] = shard_block_id;
}

ExtMessagePool::NativeShardTopFingerprint ExtMessagePool::native_reconciliation_state_fingerprint(
    const td::Ref<MasterchainState> &state) const {
  NativeShardTopFingerprint fingerprint;
  if (state.is_null()) {
    return fingerprint;
  }
  for (const auto &shard : state->get_shards()) {
    auto top = shard->top_block_id();
    if (top.shard_full().workchain == basechainId) {
      fingerprint.emplace(top.shard_full(), std::move(top));
    }
  }
  return fingerprint;
}

bool ExtMessagePool::should_skip_native_reconciliation_state(const NativeShardTopFingerprint &fingerprint) {
  if (!native_reconciliation_successful_state_fingerprint_ ||
      native_reconciliation_successful_state_fingerprint_.value() != fingerprint) {
    return false;
  }
  ++native_reconciliation_unchanged_state_skips_;
  return true;
}

void ExtMessagePool::record_successful_native_reconciliation_state(NativeShardTopFingerprint fingerprint) {
  native_reconciliation_successful_state_fingerprint_ = std::move(fingerprint);
}

td::uint64 ExtMessagePool::erase_processed_native_messages(NativeMessageProcessResult processed) {
  if (processed.expired_suffix_pruned != 0) {
    ++native_expiry_suffix_events_;
    native_expiry_suffix_pruned_ += processed.expired_suffix_pruned;
  }
  td::uint64 erased = 0;
  for (const auto &hash : processed.obsolete_hashes) {
    auto pool_it = ext_messages_hashes_.find(hash);
    if (pool_it != ext_messages_hashes_.end() && erase_message(pool_it->second.first, pool_it->second.second)) {
      ++erased;
    }
  }
  return erased;
}

td::uint64 ExtMessagePool::prune_expired_native_suffix(const NativeAddress &address, td::uint64 from_nonce,
                                                       td::Slice reason) {
  std::vector<std::pair<td::uint64, ExtMessage::Hash>> suffix;
  auto account = native_accounts_.find(address);
  if (account == native_accounts_.end()) {
    return 0;
  }
  for (auto it = account->second.messages.lower_bound(from_nonce); it != account->second.messages.end(); ++it) {
    suffix.emplace_back(it->first, it->second.hash);
  }

  td::uint64 pruned = 0;
  for (const auto &[nonce, hash] : suffix) {
    account = native_accounts_.find(address);
    if (account == native_accounts_.end()) {
      break;
    }
    auto reservation = account->second.messages.find(nonce);
    if (reservation == account->second.messages.end() || reservation->second.hash != hash) {
      continue;
    }
    if (reservation->second.allow_broadcast_promise) {
      reservation->second.allow_broadcast_promise.set_error(td::Status::Error(reason));
    }
    reservation->second.insertion_failed(reason);

    bool removed = false;
    auto pool_it = ext_messages_hashes_.find(hash);
    if (pool_it != ext_messages_hashes_.end()) {
      removed = erase_message(pool_it->second.first, pool_it->second.second, false);
    }
    if (!removed) {
      account = native_accounts_.find(address);
      if (account != native_accounts_.end()) {
        reservation = account->second.messages.find(nonce);
        if (reservation != account->second.messages.end() && reservation->second.hash == hash) {
          account->second.messages.erase(reservation);
          if (account->second.messages.empty()) {
            native_accounts_.erase(account);
          }
          prune_native_reconciliation_target_if_idle(address);
          removed = true;
        }
      }
    }
    if (removed) {
      ++pruned;
    }
  }
  if (pruned != 0) {
    ++native_expiry_suffix_events_;
    native_expiry_suffix_pruned_ += pruned;
    std::set<NativeAddress> changed_source{address};
    wake_native_callbacks(&changed_source);
  }
  return pruned;
}

td::Result<bool> ExtMessagePool::apply_canonical_native_account_state(const NativeAddress &address,
                                                                       td::uint64 native_nonce,
                                                                       td::uint64 balance, UnixTime utime,
                                                                       LogicalTime lt) {
  auto &watermark = native_nonce_watermarks_[address];
  const auto previous_nonce = watermark.observed_next_nonce;
  const auto previous_revision = watermark.revision;
  if (!watermark.observe_account_state(native_nonce, balance, lt)) {
    return td::Status::Error(ErrorCode::notready,
                             "native account state predates the latest observed canonical state");
  }
  const auto canonical_nonce = watermark.observed_next_nonce;
  const auto canonical_balance = watermark.observed_balance.value();

  NativeMessageProcessResult processed;
  auto account_it = native_accounts_.find(address);
  if (account_it != native_accounts_.end()) {
    processed = account_it->second.process_messages(canonical_nonce, utime);
  }
  auto purged = erase_processed_native_messages(std::move(processed));

  // A canonical balance update changes the revision used to protect admission
  // across signature verification. Existing pending messages are still valid
  // when their ordered reservation prefix fits the new balance; rebase that
  // prefix atomically. A stale in-flight (uncommitted) reservation cannot be
  // blessed without repeating verification, so it is also a tail boundary.
  // Once either boundary is reached, erase it and every later reservation.
  // Keeping a suffix behind the rejected nonce would create a scheduler head
  // hole and can permanently stall the source.
  td::uint64 rebased = 0;
  enum class TailReason { none, unaffordable, stale_uncommitted };
  TailReason tail_reason = TailReason::none;
  std::vector<std::pair<td::uint64, ExtMessage::Hash>> pruned_tail;
  account_it = native_accounts_.find(address);
  if (account_it != native_accounts_.end()) {
    td::uint64 prefix_amount = 0;
    for (auto &[nonce, message] : account_it->second.messages) {
      const auto required = message.amount + message.fee;
      if (tail_reason == TailReason::none && !message.committed &&
          message.account_revision != watermark.revision) {
        tail_reason = TailReason::stale_uncommitted;
      }
      if (tail_reason == TailReason::none &&
          (required < message.amount || required > canonical_balance - prefix_amount)) {
        tail_reason = TailReason::unaffordable;
      }
      if (tail_reason != TailReason::none) {
        pruned_tail.emplace_back(nonce, message.hash);
        continue;
      }
      prefix_amount += required;
      if (message.committed && message.account_revision != watermark.revision) {
        message.account_revision = watermark.revision;
        ++rebased;
      }
    }
  }

  td::uint64 tail_pruned = 0;
  const td::Slice tail_reason_text =
      tail_reason == TailReason::stale_uncommitted
          ? td::Slice("native transfer verification became stale after canonical account update")
          : td::Slice("native transfer became unaffordable after canonical balance update");
  for (const auto &[nonce, hash] : pruned_tail) {
    auto current_account = native_accounts_.find(address);
    if (current_account == native_accounts_.end()) {
      continue;
    }
    auto reservation = current_account->second.messages.find(nonce);
    if (reservation == current_account->second.messages.end() || reservation->second.hash != hash) {
      continue;
    }
    if (reservation->second.allow_broadcast_promise) {
      reservation->second.allow_broadcast_promise.set_error(td::Status::Error(tail_reason_text));
    }
    reservation->second.insertion_failed(tail_reason_text);

    bool removed = false;
    auto pool_it = ext_messages_hashes_.find(hash);
    if (pool_it != ext_messages_hashes_.end()) {
      removed = erase_message(pool_it->second.first, pool_it->second.second);
    }
    if (!removed) {
      current_account = native_accounts_.find(address);
      if (current_account != native_accounts_.end()) {
        reservation = current_account->second.messages.find(nonce);
        if (reservation != current_account->second.messages.end() && reservation->second.hash == hash) {
          current_account->second.messages.erase(reservation);
          if (current_account->second.messages.empty()) {
            native_accounts_.erase(current_account);
          }
          prune_native_reconciliation_target_if_idle(address);
          removed = true;
        }
      }
    }
    if (removed) {
      ++tail_pruned;
    }
  }

  account_it = native_accounts_.find(address);
  if (account_it != native_accounts_.end() && account_it->second.messages.empty()) {
    native_accounts_.erase(account_it);
  }

  auto tracked_it = locally_accepted_native_nonces_.find(address);
  if (tracked_it != locally_accepted_native_nonces_.end() && canonical_nonce > tracked_it->second) {
    locally_accepted_native_nonces_.erase(tracked_it);
  }
  prune_native_reconciliation_target_if_idle(address);
  native_reconciliation_messages_purged_ += purged;
  native_reconciliation_rebased_reservations_ += rebased;
  if (tail_reason == TailReason::unaffordable) {
    native_reconciliation_unaffordable_tail_pruned_ += tail_pruned;
  } else if (tail_reason == TailReason::stale_uncommitted) {
    native_reconciliation_stale_uncommitted_tail_pruned_ += tail_pruned;
  }
  if (canonical_nonce > previous_nonce) {
    ++native_reconciliation_sources_advanced_;
  }
  return watermark.revision != previous_revision || purged != 0 || rebased != 0 || tail_pruned != 0;
}

void ExtMessagePool::erase_external_messages(std::vector<ExtMessage::Hash> to_delete) {
  applied_ext_msgs_delete_requests_ += to_delete.size();
  for (auto &hash : to_delete) {
    auto it = ext_messages_hashes_norm_.find(hash);
    if (it != ext_messages_hashes_norm_.end()) {
      auto ids = it->second;
      for (const auto &message_id : ids) {
        if (erase_message(message_id.priority, message_id.id)) {
          ++applied_ext_msgs_deleted_;
        }
      }
    }
  }
}

bool ExtMessagePool::erase_message(int priority, const MessageId &id, bool prune_expired_suffix) {
  auto it_priority = ext_msgs_.find(priority);
  if (it_priority == ext_msgs_.end()) {
    return false;
  }
  auto &msgs = it_priority->second;
  auto msg_opt = msgs.ext_messages_.find(id);
  if (!msg_opt) {
    return false;
  }

  auto address = msg_opt.value()->address();
  auto hash_norm = msg_opt.value()->hash_norm;
  auto native_nonce = msg_opt.value()->native_nonce;
  if (prune_expired_suffix && native_nonce && msg_opt.value()->expired()) {
    auto native_it = native_accounts_.find(address);
    if (native_it != native_accounts_.end()) {
      auto reservation = native_it->second.messages.find(native_nonce.value());
      if (reservation != native_it->second.messages.end() &&
          reservation->second.hash == msg_opt.value()->message->hash()) {
        return prune_expired_native_suffix(
                   address, native_nonce.value(),
                   "native transfer retention expired; removed this nonce and its pending suffix") != 0;
      }
    }
  }
  if (native_nonce) {
    msgs.native_messages_ =
        msgs.native_messages_.erase(NativeMessageId{native_nonce.value(), id.dst, id.hash});
    auto native_it = native_accounts_.find(address);
    if (native_it != native_accounts_.end()) {
      auto reservation_it = native_it->second.messages.find(native_nonce.value());
      if (reservation_it != native_it->second.messages.end() &&
          reservation_it->second.hash == msg_opt.value()->message->hash()) {
        reservation_it->second.insertion_failed("native message was removed from the mempool");
        native_it->second.messages.erase(reservation_it);
      }
      if (native_it->second.messages.empty()) {
        native_accounts_.erase(native_it);
      }
    }
    prune_native_reconciliation_target_if_idle(address);
  } else {
    msgs.generic_messages_ = msgs.generic_messages_.erase(id);
  }
  msgs.ext_addr_messages_[address].erase(id.hash);
  msgs.ext_messages_ = msgs.ext_messages_.erase(id);
  ext_messages_hashes_.erase(id.hash);

  auto it_norm = ext_messages_hashes_norm_.find(hash_norm);
  if (it_norm != ext_messages_hashes_norm_.end()) {
    it_norm->second.erase(NormalizedMessageId{priority, id});
    if (it_norm->second.empty()) {
      ext_messages_hashes_norm_.erase(it_norm);
    }
  }
  return true;
}

std::vector<std::pair<std::string, std::string>> ExtMessagePool::prepare_stats() {
  std::vector<std::pair<std::string, std::string>> vec;
  vec.emplace_back("total.ext_msg_check",
                   PSTRING() << "ok:" << total_check_ext_messages_ok_ << " error:" << total_check_ext_messages_error_);
  vec.emplace_back("total.ext_msg_applied_cleanup", PSTRING() << "requested:" << applied_ext_msgs_delete_requests_
                                                              << " deleted:" << applied_ext_msgs_deleted_);
  td::uint64 mempool_total = 0;
  td::uint64 mempool_active = 0;
  td::uint64 mempool_native = 0;
  for (const auto &[_, msgs] : ext_msgs_) {
    auto iterator = msgs.ext_messages_.in_order();
    while (auto item = iterator.next()) {
      auto [__, msg] = std::move(item.value());
      ++mempool_total;
      if (msg->active) {
        ++mempool_active;
      }
      if (msg->native_nonce) {
        ++mempool_native;
      }
    }
  }
  td::uint64 native_pending = 0;
  for (const auto &[_, info] : native_accounts_) {
    native_pending += info.messages.size();
  }
  td::uint64 head_ready_sources = 0;
  td::uint64 head_missing_watermark_sources = 0;
  td::uint64 head_missing_nonce_sources = 0;
  td::uint64 head_uncommitted_sources = 0;
  td::uint64 head_missing_hash_sources = 0;
  td::uint64 head_missing_priority_sources = 0;
  td::uint64 head_missing_message_sources = 0;
  td::uint64 head_nonce_mismatch_sources = 0;
  td::uint64 head_stale_revision_sources = 0;
  td::uint64 max_nonce_gap = 0;
  for (const auto &[address, info] : native_accounts_) {
    if (info.messages.empty()) {
      continue;
    }
    auto watermark = native_nonce_watermarks_.find(address);
    if (watermark == native_nonce_watermarks_.end()) {
      ++head_missing_watermark_sources;
      continue;
    }
    auto first_nonce = watermark->second.first_unconsumed_nonce();
    if (!first_nonce) {
      ++head_missing_nonce_sources;
      continue;
    }
    auto reservation = info.messages.lower_bound(first_nonce.value());
    if (reservation == info.messages.end() || reservation->first != first_nonce.value()) {
      ++head_missing_nonce_sources;
      if (reservation != info.messages.end() && reservation->first > first_nonce.value()) {
        max_nonce_gap = std::max(max_nonce_gap, reservation->first - first_nonce.value());
      }
      continue;
    }
    if (!reservation->second.committed) {
      ++head_uncommitted_sources;
      continue;
    }
    auto pool_entry = ext_messages_hashes_.find(reservation->second.hash);
    if (pool_entry == ext_messages_hashes_.end()) {
      ++head_missing_hash_sources;
      continue;
    }
    auto priority = ext_msgs_.find(pool_entry->second.first);
    if (priority == ext_msgs_.end()) {
      ++head_missing_priority_sources;
      continue;
    }
    auto message = priority->second.ext_messages_.find(pool_entry->second.second);
    if (!message) {
      ++head_missing_message_sources;
      continue;
    }
    if (!message.value()->native_nonce || message.value()->native_nonce.value() != first_nonce.value()) {
      ++head_nonce_mismatch_sources;
      continue;
    }
    if (reservation->second.account_revision != watermark->second.revision) {
      ++head_stale_revision_sources;
      continue;
    }
    ++head_ready_sources;
  }
  vec.emplace_back("total.ext_msg_mempool", PSTRING() << "messages:" << mempool_total << " active:" << mempool_active
                                                      << " native:" << mempool_native
                                                      << " priorities:" << ext_msgs_.size());
  vec.emplace_back("total.ext_msg_native_pending", PSTRING() << "accounts:" << native_accounts_.size()
                                                             << " messages:" << native_pending
                                                             << " nonce_watermarks:"
                                                             << native_nonce_watermarks_.size());
  vec.emplace_back(
      "total.ext_msg_native_head_state",
      PSTRING() << "ready_sources:" << head_ready_sources
                << " missing_watermark_sources:" << head_missing_watermark_sources
                << " missing_nonce_sources:" << head_missing_nonce_sources
                << " uncommitted_sources:" << head_uncommitted_sources
                << " missing_hash_sources:" << head_missing_hash_sources
                << " missing_priority_sources:" << head_missing_priority_sources
                << " missing_message_sources:" << head_missing_message_sources
                << " nonce_mismatch_sources:" << head_nonce_mismatch_sources
                << " stale_revision_sources:" << head_stale_revision_sources
                << " max_nonce_gap:" << max_nonce_gap);
  vec.emplace_back("total.ext_msg_native_config", PSTRING() << "collator_queue_limit:"
                                                            << native_collator_queue_limit_ << " max_retention_s:"
                                                            << native_mempool_max_ttl_);
  vec.emplace_back(
      "total.ext_msg_batch_admission",
      PSTRING() << "batches:" << native_batch_count_ << " messages:" << native_batch_messages_
                << " unique:" << native_batch_unique_messages_
                << " shard_state_requests:" << native_batch_shard_state_requests_
                << " shard_manager_waits:" << native_batch_shard_manager_waits_
                << " shard_fetches:" << native_batch_shard_manager_waits_
                << " shard_cache_hits:" << native_batch_shard_cache_hits_
                << " shard_cache_fills:" << native_batch_shard_cache_fills_
                << " shard_cache_fill_races:" << native_batch_shard_cache_fill_races_
                << " shard_cache_fill_conflicts:" << native_batch_shard_cache_fill_conflicts_
                << " shard_cache_generation_resets:" << native_batch_shard_cache_generation_resets_
                << " shard_cache_stale_generation_fill_skips:"
                << native_batch_shard_cache_stale_generation_fill_skips_
                << " shard_cache_wrong_id:" << native_batch_shard_cache_wrong_id_
                << " shard_cache_invalid_header:" << native_batch_shard_cache_invalid_header_
                << " shard_miss_errors:" << native_batch_shard_miss_errors_
                << " shard_fetch_errors:" << native_batch_shard_miss_errors_
                << " shard_manager_wait_errors:" << native_batch_shard_manager_wait_errors_
                << " shard_manager_wait_timeouts:" << native_batch_shard_manager_wait_timeouts_
                << " shard_manager_wait_notready:" << native_batch_shard_manager_wait_notready_
                << " shard_manager_wait_other_errors:" << native_batch_shard_manager_wait_other_errors_
                << " shard_manager_wait_late_results:" << native_batch_shard_manager_wait_late_results_
                << " shard_cache_entries:" << native_admission_shard_cache_.shard_views.size()
                << " shard_cache_peak_entries:" << native_batch_shard_cache_peak_entries_
                << " account_lookups:" << native_batch_account_lookups_ << " accepted:" << native_batch_accepted_
                << " rejected:" << native_batch_rejected_ << " mc_state_pins:" << native_batch_mc_state_pins_
                << " pinned_mc_seqno:" << native_batch_last_pinned_mc_seqno_
                << " pinned_shard_seqno:" << native_batch_last_pinned_shard_seqno_
                << " max_mc_shard_utime_lag_s:" << native_batch_max_mc_shard_utime_lag_s_
                << " watermark_lag_rejections:" << native_batch_watermark_lag_rejections_
                << " max_watermark_nonce_lag:" << native_batch_max_watermark_nonce_lag_
                << " ignored_mc_state_updates:" << native_batch_ignored_mc_state_updates_);
  vec.emplace_back(
      "total.ext_msg_native_reconciliation",
      PSTRING() << "tracked_candidates:" << native_reconciliation_tracked_candidates_
                << " tracked_messages:" << native_reconciliation_tracked_messages_
                << " pending_sources:" << locally_accepted_native_nonces_.size()
                << " runs:" << native_reconciliation_runs_
                << " state_fetches:" << native_reconciliation_state_fetches_
                << " account_lookups:" << native_reconciliation_account_lookups_
                << " sources_advanced:" << native_reconciliation_sources_advanced_
                << " messages_purged:" << native_reconciliation_messages_purged_
                << " failures:" << native_reconciliation_failures_
                << " unchanged_state_skips:" << native_reconciliation_unchanged_state_skips_
                << " unchanged_top_skips:" << native_reconciliation_unchanged_top_skips_
                << " unchanged_source_skips:" << native_reconciliation_unchanged_source_skips_
                << " rebased_reservations:" << native_reconciliation_rebased_reservations_
                << " unaffordable_tail_pruned:" << native_reconciliation_unaffordable_tail_pruned_
                << " stale_uncommitted_tail_pruned:"
                << native_reconciliation_stale_uncommitted_tail_pruned_
                << " expiry_suffix_events:" << native_expiry_suffix_events_
                << " expiry_suffix_pruned:" << native_expiry_suffix_pruned_
                << " exact_retry_preserved_stale_revision:"
                << native_exact_retry_preserved_stale_revision_
                << " last_mc_seqno:" << native_reconciliation_last_mc_seqno_
                << " last_shard_seqno:" << native_reconciliation_last_shard_seqno_);
  vec.emplace_back(
      "total.ext_msg_native_scheduler",
      PSTRING() << "installs:" << native_queue_counters_.installs
                << " masterchain_installs:" << native_queue_counters_.masterchain_installs
                << " scanned:" << native_queue_counters_.scanned << " selected:" << native_queue_counters_.selected
                << " active:" << native_queue_counters_.active << " inactive:" << native_queue_counters_.inactive
                << " excluded:" << native_queue_counters_.excluded << " expired:" << native_queue_counters_.expired
                << " delivered_skips:" << native_queue_counters_.already_delivered
                << " ready_sources:" << native_queue_counters_.ready_sources
                << " head_gaps:" << native_queue_counters_.head_gaps
                << " head_missing_watermark:" << native_queue_counters_.head_missing_watermark
                << " head_missing_nonce:" << native_queue_counters_.head_missing_nonce
                << " head_uncommitted:" << native_queue_counters_.head_uncommitted
                << " head_missing_hash_index:" << native_queue_counters_.head_missing_hash_index
                << " head_missing_priority:" << native_queue_counters_.head_missing_priority
                << " head_missing_message:" << native_queue_counters_.head_missing_message
                << " head_nonce_mismatch:" << native_queue_counters_.head_nonce_mismatch
                << " speculative_exhausted:" << native_queue_counters_.speculative_exhausted
                << " runs:" << native_queue_counters_.runs
                << " run_messages:" << native_queue_counters_.run_messages
                << " max_run_size:" << native_queue_counters_.max_run_size
                << " delayed:" << native_queue_counters_.delayed
                << " reactivated:" << native_queue_counters_.reactivated
                << " reactivation_wakes:" << native_queue_counters_.reactivation_wakes
                << " scheduler_builds:" << native_queue_counters_.scheduler_builds
                << " source_scans:" << native_queue_counters_.source_scans
                << " source_refreshes:" << native_queue_counters_.source_refreshes
                << " source_probes:" << native_queue_counters_.source_probes
                << " stale_ready_tokens:" << native_queue_counters_.stale_ready_tokens);
  std::lock_guard transport_lock(native_transport_telemetry_->accounting_mutex);
  auto selected = native_transport_telemetry_->selected.load(std::memory_order_relaxed);
  auto push_completed = native_transport_telemetry_->pushed.load(std::memory_order_relaxed);
  auto push_reserved = native_transport_telemetry_->push_reserved.load(std::memory_order_relaxed);
  // A reserved batch may already be visible to the consumer in queue-sized
  // prefixes even though push_many_bounded has not resumed to publish its exact
  // result. Export this publication bound as `pushed`; the monotonic exact
  // result remains available separately as `push_completed`.
  auto pushed = push_completed + push_reserved;
  auto consumed = native_transport_telemetry_->consumed.load(std::memory_order_relaxed);
  auto queued_discarded = native_transport_telemetry_->queued_discarded.load(std::memory_order_relaxed);
  auto unpushed_discarded = native_transport_telemetry_->unpushed_discarded.load(std::memory_order_relaxed);
  auto queued_total = pushed > consumed ? pushed - consumed : 0;
  auto unpushed_total = selected > pushed ? selected - pushed : 0;
  auto live_queued = queued_total > queued_discarded ? queued_total - queued_discarded : 0;
  auto live_unpushed = unpushed_total > unpushed_discarded ? unpushed_total - unpushed_discarded : 0;
  auto pending = live_queued + live_unpushed;
  auto push_batches = native_transport_telemetry_->push_batches.load(std::memory_order_relaxed);
  auto pop_batches = native_transport_telemetry_->pop_batches.load(std::memory_order_relaxed);
  vec.emplace_back(
      "total.ext_msg_native_transport",
      PSTRING() << "selected:" << selected << " pushed:" << pushed << " consumed:" << consumed
                << " push_completed:" << push_completed << " push_reserved:" << push_reserved
                << " pending:" << pending << " live_queued:" << live_queued
                << " live_unpushed:" << live_unpushed
                << " queued_discarded:" << queued_discarded
                << " unpushed_discarded:" << unpushed_discarded
                << " cancel_discarded:" << queued_discarded + unpushed_discarded
                << " high_water:" << native_transport_telemetry_->high_water.load(std::memory_order_relaxed)
                << " push_batches:" << push_batches
                << " push_batch_items:"
                << native_transport_telemetry_->push_batch_items.load(std::memory_order_relaxed)
                << " max_push_batch:"
                << native_transport_telemetry_->max_push_batch.load(std::memory_order_relaxed)
                << " pop_batches:" << pop_batches
                << " pop_batch_items:"
                << native_transport_telemetry_->pop_batch_items.load(std::memory_order_relaxed)
                << " max_pop_batch:"
                << native_transport_telemetry_->max_pop_batch.load(std::memory_order_relaxed)
                << " producer_empty:"
                << native_transport_telemetry_->producer_empty.load(std::memory_order_relaxed)
                << " consumer_empty:"
                << native_transport_telemetry_->consumer_empty.load(std::memory_order_relaxed));
  return vec;
}

void ExtMessagePool::alarm() {
  reactivate_due_native_messages(td::Timestamp::now());
  if (cleanup_mempool_at_.is_in_past()) {
    cleanup_external_messages(ShardIdFull{masterchainId, shardIdAll});
    cleanup_external_messages(ShardIdFull{basechainId, shardIdAll});
    cleanup_mempool_at_ = td::Timestamp::in(250.0);
  }
  alarm_timestamp().relax(cleanup_mempool_at_);
  if (!native_reactivations_.empty()) {
    alarm_timestamp().relax(native_reactivations_.begin()->first);
  }
  std::erase_if(callbacks_, [&](const std::shared_ptr<InstalledCallback> &callback) -> bool {
    if (callback->callback->timeout && callback->callback->timeout.is_in_past()) {
      cancel_callback_delivery(callback);
      return true;
    }
    alarm_timestamp().relax(callback->callback->timeout);
    return false;
  });
}

td::Status ExtMessagePool::add_message_to_mempool(td::Ref<ExtMessage> message, int priority,
                                                  td::optional<td::uint32> msg_seqno,
                                                  const block::NativeTransfer *native_transfer) {
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  auto address = std::make_pair(wc, addr);
  auto &msgs = ext_msgs_[priority];
  auto msg = std::make_shared<MempoolMsg>(message);
  msg->msg_seqno = msg_seqno;
  td::optional<block::NativeTransfer> parsed_native_transfer;
  if (native_transfer == nullptr) {
    auto native_transfer_res = block::NativeTransfer::unpack_external(message->root_cell());
    if (native_transfer_res.is_ok()) {
      parsed_native_transfer = native_transfer_res.move_as_ok();
      native_transfer = &parsed_native_transfer.value();
    }
  }
  if (native_transfer != nullptr) {
    const auto &transfer = *native_transfer;
    msg->native_nonce = transfer.nonce;
    auto watermark_it = native_nonce_watermarks_.find(address);
    if (watermark_it != native_nonce_watermarks_.end() && watermark_it->second.is_consumed(transfer.nonce)) {
      return td::Status::Error(PSTRING() << "native nonce " << transfer.nonce
                                         << " was consumed before mempool insertion");
    }
    auto account_it = native_accounts_.find(address);
    if (account_it == native_accounts_.end()) {
      return td::Status::Error("native message reservation disappeared before mempool insertion");
    }
    auto reservation_it = account_it->second.messages.find(transfer.nonce);
    if (reservation_it == account_it->second.messages.end()) {
      return td::Status::Error("native message reservation disappeared before mempool insertion");
    }
    if (watermark_it == native_nonce_watermarks_.end() ||
        reservation_it->second.account_revision != watermark_it->second.revision) {
      return td::Status::Error(ErrorCode::notready,
                               "native account changed before mempool insertion; retry admission");
    }
    auto remaining = std::max(0.001, static_cast<double>(transfer.valid_until) - td::Clocks::system());
    msg->set_retention(std::min(remaining, static_cast<double>(native_mempool_max_ttl_)));
  }
  MessageId id{message->shard(), message->hash()};
  auto it2 = ext_messages_hashes_.find(id.hash);
  if (it2 != ext_messages_hashes_.end()) {
    auto [existing_priority, existing_id] = it2->second;
    auto priority_it = ext_msgs_.find(existing_priority);
    CHECK(priority_it != ext_msgs_.end());
    auto existing_message = priority_it->second.ext_messages_.find(existing_id);
    CHECK(existing_message);
    if (existing_message.value()->expired()) {
      CHECK(erase_message(existing_priority, existing_id));
      CHECK(ext_messages_hashes_.find(id.hash) == ext_messages_hashes_.end());
      it2 = ext_messages_hashes_.end();
    }
  }
  if (it2 != ext_messages_hashes_.end() && it2->second.first >= priority) {
    LOG(DEBUG) << "message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
               << " is already present in mempool at priority " << it2->second.first;
    return td::Status::OK();
  }
  if (msgs.ext_messages_.size() >= opts_->max_mempool_num()) {
    auto error = td::Status::Error(ErrorCode::notready,
                                   PSTRING() << "external message mempool is full (priority=" << priority
                                             << ", limit=" << opts_->max_mempool_num() << ")");
    LOG(INFO) << "cannot add message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority << " : "
              << error;
    return error;
  }
  auto it = msgs.ext_addr_messages_.find(address);
  if (it != msgs.ext_addr_messages_.end() && it->second.size() >= PER_ADDRESS_LIMIT) {
    auto error = td::Status::Error(ErrorCode::notready,
                                   PSTRING() << "external message per-address mempool limit reached (address=" << wc
                                             << ":" << addr.to_hex() << ", priority=" << priority
                                             << ", limit=" << PER_ADDRESS_LIMIT << ")");
    LOG(INFO) << "cannot add message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority << " : "
              << error;
    return error;
  }
  if (it2 != ext_messages_hashes_.end()) {
    int old_priority = it2->second.first;
    erase_message(old_priority, id);
  }
  auto hash_norm = msg->hash_norm;
  msgs.ext_messages_ = msgs.ext_messages_.insert(id, msg);
  if (msg->native_nonce) {
    msgs.native_messages_ =
        msgs.native_messages_.insert(NativeMessageId{msg->native_nonce.value(), id.dst, id.hash}, msg);
  } else {
    msgs.generic_messages_ = msgs.generic_messages_.insert(id, msg);
  }
  msgs.ext_addr_messages_[address].emplace(id.hash, id);
  ext_messages_hashes_[id.hash] = {priority, id};
  ext_messages_hashes_norm_[hash_norm].insert(NormalizedMessageId{priority, id});
  VLOG(VALIDATOR_DEBUG) << "adding message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority
                        << " to mempool";
  if (!msg->native_nonce) {
    std::erase_if(callbacks_, [&](const std::shared_ptr<InstalledCallback> &callback) -> bool {
      if (callback->callback->cancellation_token.check().is_error()) {
        cancel_callback_delivery(callback);
        return true;
      }
      if (shard_contains(callback->callback->shard, message->shard()) &&
          !std::binary_search(callback->callback->excluded_messages.begin(),
                              callback->callback->excluded_messages.end(), message->hash()) &&
          (callback->callback->shard.workchain != masterchainId ||
           callback->generic_selected < STANDARD_COLLATOR_QUEUE_LIMIT)) {
        ++callback->generic_selected;
        begin_callback_epoch(callback);
        enqueue_callback_item(callback, std::make_pair(message, priority), false);
        start_callback_pump(callback);
      }
      return false;
    });
  }
  return td::Status::OK();
}

td::Status ExtMessagePool::commit_checked_message(td::Ref<ExtMessage> message,
                                                  td::optional<td::uint32> msg_seqno,
                                                  const block::NativeTransfer *native_transfer) {
  auto address = std::make_pair(message->wc(), message->addr());
  td::optional<block::NativeTransfer> parsed_native_transfer;
  if (native_transfer == nullptr) {
    auto result = block::NativeTransfer::unpack_external(message->root_cell());
    if (result.is_ok()) {
      parsed_native_transfer = result.move_as_ok();
      native_transfer = &parsed_native_transfer.value();
    }
  }
  if (native_transfer != nullptr) {
    auto watermark_it = native_nonce_watermarks_.find(address);
    if (watermark_it != native_nonce_watermarks_.end() &&
        watermark_it->second.is_consumed(native_transfer->nonce)) {
      return td::Status::Error(PSTRING() << "native nonce " << native_transfer->nonce
                                         << " was consumed before mempool commit");
    }
    auto native_it = native_accounts_.find(address);
    if (native_it == native_accounts_.end()) {
      return td::Status::Error("native message reservation disappeared before mempool commit");
    }
    auto reservation_it = native_it->second.messages.find(native_transfer->nonce);
    if (reservation_it == native_it->second.messages.end()) {
      return td::Status::Error("native message reservation disappeared before mempool commit");
    }
    if (watermark_it == native_nonce_watermarks_.end() ||
        reservation_it->second.account_revision != watermark_it->second.revision) {
      return td::Status::Error(ErrorCode::notready,
                               "native account changed before mempool commit; retry admission");
    }
    NativeMessageProcessResult processed;
    CHECK(native_it->second.commit_message(native_transfer->nonce, processed));
    bool committed_message_expired =
        std::find(processed.obsolete_hashes.begin(), processed.obsolete_hashes.end(), message->hash()) !=
        processed.obsolete_hashes.end();
    erase_processed_native_messages(std::move(processed));
    if (committed_message_expired) {
      return td::Status::Error("native message expired before mempool commit");
    }
    return td::Status::OK();
  }
  if (msg_seqno) {
    auto wallet_it = wallets_.find(address);
    if (wallet_it == wallets_.end() || !wallet_it->second.commit_message(msg_seqno.value())) {
      return td::Status::Error("wallet message reservation disappeared before mempool commit");
    }
  }
  return td::Status::OK();
}

void ExtMessagePool::rollback_checked_message(td::Ref<ExtMessage> message,
                                              td::optional<td::uint32> msg_seqno,
                                              const block::NativeTransfer *native_transfer) {
  auto address = std::make_pair(message->wc(), message->addr());
  td::optional<block::NativeTransfer> parsed_native_transfer;
  if (native_transfer == nullptr) {
    auto result = block::NativeTransfer::unpack_external(message->root_cell());
    if (result.is_ok()) {
      parsed_native_transfer = result.move_as_ok();
      native_transfer = &parsed_native_transfer.value();
    }
  }
  if (native_transfer != nullptr) {
    auto native_it = native_accounts_.find(address);
    if (native_it != native_accounts_.end()) {
      auto message_it = native_it->second.messages.find(native_transfer->nonce);
      if (message_it != native_it->second.messages.end()) {
        if (message_it->second.allow_broadcast_promise) {
          message_it->second.allow_broadcast_promise.set_error(
              td::Status::Error("native message was not inserted into the mempool"));
        }
        message_it->second.insertion_failed("native message was not inserted into the mempool");
        native_it->second.messages.erase(message_it);
      }
      if (native_it->second.messages.empty()) {
        native_accounts_.erase(native_it);
      }
    }
    prune_native_reconciliation_target_if_idle(address);
    return;
  }
  if (msg_seqno) {
    auto wallet_it = wallets_.find(address);
    if (wallet_it != wallets_.end()) {
      auto message_it = wallet_it->second.messages.find(msg_seqno.value());
      if (message_it != wallet_it->second.messages.end()) {
        if (message_it->second.allow_broadcast_promise) {
          message_it->second.allow_broadcast_promise.set_error(
              td::Status::Error("wallet message was not inserted into the mempool"));
        }
        wallet_it->second.messages.erase(message_it);
      }
      if (wallet_it->second.messages.empty()) {
        wallets_.erase(wallet_it);
      }
    }
  }
}

td::actor::Task<ExtMessagePool::CheckResult> ExtMessagePool::check_message(td::Ref<ExtMessage> message,
                                                                           td::Timestamp deadline) {
  auto deadline_expired = [&] { return deadline && deadline.is_in_past(); };
  if (deadline_expired()) {
    co_return td::Status::Error(ErrorCode::timeout,
                                "external message admission deadline expired");
  }
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  auto [shard_acc, utime, lt, config] = co_await run_fetch_account_state(wc, addr, manager_);
  if (deadline_expired()) {
    co_return td::Status::Error(ErrorCode::timeout,
                                "external message admission deadline expired");
  }
  bool special = wc == masterchainId && config->is_special_smartcontract(addr);
  block::Account acc;
  if (!acc.unpack(shard_acc, utime, special)) {
    co_return td::Status::Error(PSLICE() << "Failed to unpack account state");
  }
  acc.block_lt = lt;

  auto [wait_allow_broadcast, allow_broadcast_promise] = td::actor::StartedTask<>::make_bridge();
  CheckResult check_result{.message = message,
                           .wait_allow_broadcast = std::move(wait_allow_broadcast),
                           .should_broadcast = true,
                           .msg_seqno = {},
                           .native_transfer = {}};

  auto native_transfer_res = block::NativeTransfer::unpack_external(message->root_cell());
  if (native_transfer_res.is_ok()) {
    auto transfer = native_transfer_res.move_as_ok();
    if (wc != basechainId || transfer.src != addr) {
      co_return td::Status::Error("native transfer is routed to the wrong source account");
    }
    if (transfer.valid_until <= (UnixTime)td::Clocks::system()) {
      co_return td::Status::Error("native transfer valid_until is in the past");
    }
    if (acc.status != block::Account::acc_uninit || !acc.is_native) {
      co_return td::Status::Error("native transfer source account must be balance-only");
    }
    auto available_balance = acc.native_balance_uint64();
    if (!available_balance) {
      co_return td::Status::Error("native transfer source balance must be uint64 grams without extra currencies");
    }
    auto native_address = std::make_pair(wc, addr);
    td::optional<td::uint64> initial_native_nonce;
    td::uint64 account_revision = 0;
    {
      auto applied = apply_canonical_native_account_state(native_address, acc.native_nonce,
                                                           available_balance.value(), utime, lt);
      if (applied.is_error()) {
        co_return applied.move_as_error();
      }
      const auto &initial_watermark = native_nonce_watermarks_.at(native_address);
      initial_native_nonce = initial_watermark.first_unconsumed_nonce();
      account_revision = initial_watermark.revision;
      if (applied.ok()) {
        std::set<NativeAddress> changed_source{native_address};
        wake_native_callbacks(&changed_source);
      }
    }
    if (!initial_native_nonce) {
      co_return td::Status::Error("native account nonce space is exhausted");
    }
    if (transfer.nonce < initial_native_nonce.value()) {
      co_return td::Status::Error(PSTRING() << "Too old native nonce: msg_nonce=" << transfer.nonce
                                            << ", account_nonce=" << initial_native_nonce.value());
    }
    if (transfer.nonce - initial_native_nonce.value() > MAX_NATIVE_NONCE_DIFF) {
      co_return td::Status::Error(PSTRING() << "Too new native nonce: msg_nonce=" << transfer.nonce
                                            << ", account_nonce=" << initial_native_nonce.value());
    }
    if (transfer.amount + transfer.fee < transfer.amount) {
      co_return td::Status::Error("native transfer amount and fee overflow");
    }
    td::uint64 required_amount = transfer.amount + transfer.fee;
    block::CurrencyCollection required{td::make_refint(required_amount)};
    if (!(acc.balance >= required)) {
      co_return td::Status::Error("native transfer has insufficient source balance");
    }
    CHECK(!native_signature_verifiers_.empty());
    auto& verifier =
        native_signature_verifiers_[native_signature_verifier_cursor_++ % native_signature_verifiers_.size()];
    auto signature_result = co_await td::actor::ask(verifier, &NativeSignatureVerifier::verify, transfer,
                                                    config->get_zerostate_id().root_hash)
                                .wrap();
    if (signature_result.is_error()) {
      co_return signature_result.move_as_error();
    }
    if (deadline_expired()) {
      co_return td::Status::Error(ErrorCode::timeout,
                                  "external message admission deadline expired");
    }

    co_return co_await reserve_verified_native_message(message, std::move(transfer), available_balance.value(),
                                                       account_revision, utime, deadline);
  }

  const WalletMessageProcessor *wallet =
      acc.code.not_null() ? WalletMessageProcessor::get(acc.code->get_hash().bits()) : nullptr;
  if (wallet != nullptr) {
    check_result.msg_seqno =
        co_await check_message_to_wallet(message, wallet, std::move(acc), utime, lt, std::move(config),
                                         std::move(allow_broadcast_promise));
    co_return check_result;
  }
  wallets_.erase({wc, addr});
  co_await ExtMessageQ::run_message_on_account(wc, &acc, utime, lt + 1, message->root_cell(), std::move(config));
  if (deadline_expired()) {
    co_return td::Status::Error(ErrorCode::timeout,
                                "external message admission deadline expired");
  }
  allow_broadcast_promise.set_value(td::Unit{});
  co_return check_result;
}

td::actor::Task<ExtMessagePool::CheckResult> ExtMessagePool::reserve_verified_native_message(
    td::Ref<ExtMessage> message, block::NativeTransfer transfer, td::uint64 available_balance,
    td::uint64 account_revision, UnixTime utime, td::Timestamp deadline) {
  if (deadline && deadline.is_in_past()) {
    co_return td::Status::Error(ErrorCode::timeout, "external message admission deadline expired");
  }
  auto native_address = std::make_pair(message->wc(), message->addr());
  td::optional<td::uint64> current_native_nonce;
  {
    auto &current_watermark = native_nonce_watermarks_[native_address];
    if (current_watermark.revision != account_revision) {
      co_return td::Status::Error(ErrorCode::notready,
                                  "native account changed during signature verification; retry admission");
    }
    current_native_nonce = current_watermark.first_unconsumed_nonce();
  }
  if (!current_native_nonce) {
    co_return td::Status::Error("native account nonce space is exhausted");
  }
  if (transfer.nonce < current_native_nonce.value()) {
    co_return td::Status::Error(PSTRING() << "Too old native nonce after signature verification: msg_nonce="
                                          << transfer.nonce << ", account_nonce=" << current_native_nonce.value());
  }
  auto existing_native_info = native_accounts_.find(native_address);
  if (existing_native_info != native_accounts_.end()) {
    auto processed = existing_native_info->second.process_messages(current_native_nonce.value(), utime);
    erase_processed_native_messages(std::move(processed));
    auto cleanup_it = native_accounts_.find(native_address);
    if (cleanup_it != native_accounts_.end() && cleanup_it->second.messages.empty()) {
      native_accounts_.erase(cleanup_it);
    }
    prune_native_reconciliation_target_if_idle(native_address);
  }

  auto [wait_allow_broadcast, allow_broadcast_promise] = td::actor::StartedTask<>::make_bridge();
  CheckResult check_result{.message = message,
                           .wait_allow_broadcast = std::move(wait_allow_broadcast),
                           .should_broadcast = true,
                           .msg_seqno = {},
                           .native_transfer = transfer};
  td::uint64 required_amount = transfer.amount + transfer.fee;
  if (required_amount < transfer.amount) {
    co_return td::Status::Error("native transfer amount and fee overflow");
  }
  td::actor::StartedTask<> wait_for_insertion;
  {
    auto &native_info = native_accounts_[native_address];
    auto pending_it = native_info.messages.find(transfer.nonce);
    if (pending_it != native_info.messages.end()) {
      if (pending_it->second.hash != message->hash()) {
        co_return td::Status::Error(PSTRING() << "Duplicate native nonce " << transfer.nonce);
      }
      if (pending_it->second.committed) {
        allow_broadcast_promise.set_value(td::Unit{});
        check_result.should_broadcast = false;
        co_return check_result;
      }
      auto [waiter, waiter_promise] = td::actor::StartedTask<>::make_bridge();
      pending_it->second.insertion_waiters.emplace_back(std::move(waiter_promise));
      wait_for_insertion = std::move(waiter);
    } else {
      td::uint64 reserved_amount =
          native_info.reserved_amount_before(current_native_nonce.value(), transfer.nonce);
      if (reserved_amount + required_amount < reserved_amount) {
        if (native_info.messages.empty()) {
          native_accounts_.erase(native_address);
          prune_native_reconciliation_target_if_idle(native_address);
        }
        co_return td::Status::Error("native transfer pending amount overflow");
      }
      if (reserved_amount + required_amount > available_balance) {
        if (native_info.messages.empty()) {
          native_accounts_.erase(native_address);
          prune_native_reconciliation_target_if_idle(native_address);
        }
        co_return td::Status::Error("native transfer has insufficient source balance");
      }
      auto insert_result = native_info.messages.try_emplace(transfer.nonce);
      if (!insert_result.second) {
        co_return td::Status::Error(PSTRING() << "Duplicate native nonce " << transfer.nonce);
      }
      auto &inserted = insert_result.first->second;
      inserted.hash = message->hash();
      inserted.amount = transfer.amount;
      inserted.fee = transfer.fee;
      inserted.valid_until = transfer.valid_until;
      inserted.account_revision = account_revision;
      inserted.allow_broadcast_promise = std::move(allow_broadcast_promise);
      inserted.committed = false;

      std::vector<std::pair<td::uint64, ExtMessage::Hash>> unaffordable_tail;
      td::uint64 prefix_amount = 0;
      bool tail_started = false;
      for (const auto &[nonce, pending] : native_info.messages) {
        if (nonce < current_native_nonce.value()) {
          continue;
        }
        td::uint64 amount = pending.amount + pending.fee;
        if (tail_started || amount < pending.amount || prefix_amount + amount < prefix_amount ||
            prefix_amount + amount > available_balance) {
          tail_started = true;
          unaffordable_tail.emplace_back(nonce, pending.hash);
        } else {
          prefix_amount += amount;
        }
      }
      CHECK(std::none_of(unaffordable_tail.begin(), unaffordable_tail.end(),
                         [&](const auto &entry) { return entry.second == message->hash(); }));
      for (const auto &[nonce, hash] : unaffordable_tail) {
        auto pool_it = ext_messages_hashes_.find(hash);
        if (pool_it != ext_messages_hashes_.end()) {
          erase_message(pool_it->second.first, pool_it->second.second);
          continue;
        }
        auto account_it = native_accounts_.find(native_address);
        if (account_it == native_accounts_.end()) {
          continue;
        }
        auto message_it = account_it->second.messages.find(nonce);
        if (message_it == account_it->second.messages.end() || message_it->second.hash != hash) {
          continue;
        }
        if (message_it->second.allow_broadcast_promise) {
          message_it->second.allow_broadcast_promise.set_error(td::Status::Error(
              "native transfer superseded by a lower nonce due to insufficient reserved balance"));
        }
        message_it->second.insertion_failed(
            "native transfer superseded by a lower nonce due to insufficient reserved balance");
        account_it->second.messages.erase(message_it);
        if (account_it->second.messages.empty()) {
          native_accounts_.erase(account_it);
        }
        prune_native_reconciliation_target_if_idle(native_address);
      }
      co_return check_result;
    }
  }

  auto insertion_result = co_await std::move(wait_for_insertion).wrap();
  if (insertion_result.is_error()) {
    co_return insertion_result.move_as_error();
  }
  allow_broadcast_promise.set_value(td::Unit{});
  check_result.should_broadcast = false;
  co_return check_result;
}

td::Result<td::uint32> ExtMessagePool::check_message_to_wallet(td::Ref<ExtMessage> message,
                                                               const WalletMessageProcessor *wallet, block::Account acc,
                                                               UnixTime utime, LogicalTime lt,
                                                               std::unique_ptr<block::ConfigInfo> config,
                                                               td::Promise<td::Unit> allow_broadcast_promise) {
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  LOG(DEBUG) << "Checking external message to " << wc << ":" << addr.to_hex() << ", " << wallet->name();
  TRY_RESULT(wallet_seqno, wallet->get_wallet_seqno(acc.data));
  auto &wallet_info = wallets_[{wc, addr}];
  SCOPE_EXIT {
    if (wallet_info.messages.empty()) {
      wallets_.erase({wc, addr});
    }
  };
  wallet_info.process_messages(wallet_seqno, utime);
  TRY_RESULT(parsed_message, wallet->parse_message(message->root_cell()));
  auto [msg_seqno, msg_valid_until] = parsed_message;
  LOG(DEBUG) << "External message to " << wallet->name() << ": msg_seqno=" << msg_seqno
             << ", msg_ttl=" << msg_valid_until << ", wallet_seqno=" << wallet_seqno;
  if (msg_valid_until <= (UnixTime)td::Clocks::system()) {
    return td::Status::Error("valid_until is in the past");
  }
  if (msg_seqno < wallet_seqno) {
    return td::Status::Error(PSTRING() << "Too old seqno: msg_seqno=" << msg_seqno
                                       << ", wallet_seqno=" << wallet_seqno);
  }
  if (msg_seqno - wallet_seqno > MAX_WALLET_SEQNO_DIFF) {
    return td::Status::Error(PSTRING() << "Too new seqno: msg_seqno=" << msg_seqno
                                       << ", wallet_seqno=" << wallet_seqno);
  }
  if (wallet_info.messages.contains(msg_seqno)) {
    return td::Status::Error(PSTRING() << "Duplicate msg_seqno " << msg_seqno);
  }
  TRY_RESULT_ASSIGN(acc.data, wallet->set_wallet_seqno(acc.data, msg_seqno));
  acc.storage_dict_hash = acc.orig_storage_dict_hash = {};
  TRY_STATUS(ExtMessageQ::run_message_on_account(wc, &acc, utime, lt + 1, message->root_cell(), std::move(config)));
  wallet_info.messages[msg_seqno] =
      WalletMessageInfo{.valid_until = msg_valid_until,
                        .allow_broadcast_promise = std::move(allow_broadcast_promise),
                        .committed = false};
  LOG(DEBUG) << "Checked external message to " << wc << ":" << addr.to_hex() << ", " << wallet->name();
  return msg_seqno;
}

void ExtMessagePool::WalletInfo::process_messages(td::uint32 wallet_seqno, UnixTime utime) {
  observed_seqno = std::max(observed_seqno, wallet_seqno);
  observed_utime = std::max(observed_utime, utime);
  wallet_seqno = observed_seqno;
  utime = observed_utime;
  for (auto it = messages.begin(); it != messages.end();) {
    auto &[seqno, message] = *it;
    if (seqno < wallet_seqno) {
      if (message.allow_broadcast_promise) {
        message.allow_broadcast_promise.set_error(
            td::Status::Error(PSTRING() << "Too old seqno: msg_seqno=" << seqno << ", wallet_seqno=" << wallet_seqno));
      }
      it = messages.erase(it);
      continue;
    }
    if (message.valid_until <= utime) {
      if (message.allow_broadcast_promise) {
        message.allow_broadcast_promise.set_error(td::Status::Error("valid_until is in the past"));
      }
      it = messages.erase(it);
      continue;
    }
    ++it;
  }
  for (td::uint32 seqno = wallet_seqno;; ++seqno) {
    auto it = messages.find(seqno);
    if (it == messages.end() || !it->second.committed) {
      break;
    }
    if (it->second.allow_broadcast_promise) {
      it->second.allow_broadcast_promise.set_value(td::Unit{});
    }
  }
}

bool ExtMessagePool::WalletInfo::commit_message(td::uint32 msg_seqno) {
  auto it = messages.find(msg_seqno);
  if (it == messages.end()) {
    return false;
  }
  it->second.committed = true;
  process_messages(observed_seqno, observed_utime);
  return true;
}

ExtMessagePool::NativeMessageProcessResult ExtMessagePool::NativeInfo::process_messages(td::uint64 native_nonce,
                                                                                         UnixTime utime) {
  NativeMessageProcessResult result;
  observed_nonce = std::max(observed_nonce, native_nonce);
  observed_utime = std::max(observed_utime, utime);
  native_nonce = observed_nonce;
  utime = observed_utime;
  bool expired_suffix = false;
  for (auto it = messages.begin(); it != messages.end();) {
    auto &[nonce, message] = *it;
    if (nonce < native_nonce) {
      result.obsolete_hashes.push_back(message.hash);
      if (message.allow_broadcast_promise) {
        message.allow_broadcast_promise.set_error(
            td::Status::Error(PSTRING() << "Too old native nonce: msg_nonce=" << nonce
                                        << ", account_nonce=" << native_nonce));
      }
      message.insertion_failed("native message nonce became obsolete before insertion");
      it = messages.erase(it);
      continue;
    }
    if (!expired_suffix && message.valid_until <= utime) {
      expired_suffix = true;
    }
    if (expired_suffix) {
      result.obsolete_hashes.push_back(message.hash);
      ++result.expired_suffix_pruned;
      if (message.allow_broadcast_promise) {
        message.allow_broadcast_promise.set_error(
            td::Status::Error("native transfer suffix removed after a nonce expired"));
      }
      message.insertion_failed("native transfer suffix removed after a nonce expired");
      it = messages.erase(it);
      continue;
    }
    ++it;
  }
  for (td::uint64 nonce = native_nonce;; ++nonce) {
    auto it = messages.find(nonce);
    if (it == messages.end() || !it->second.committed) {
      break;
    }
    if (it->second.allow_broadcast_promise) {
      it->second.allow_broadcast_promise.set_value(td::Unit{});
    }
    if (nonce == std::numeric_limits<td::uint64>::max()) {
      break;
    }
  }
  return result;
}

bool ExtMessagePool::NativeInfo::commit_message(td::uint64 native_nonce,
                                                NativeMessageProcessResult &processed) {
  auto it = messages.find(native_nonce);
  if (it == messages.end()) {
    return false;
  }
  it->second.committed = true;
  processed = process_messages(observed_nonce, observed_utime);
  it = messages.find(native_nonce);
  if (it != messages.end()) {
    it->second.insertion_succeeded();
  }
  return true;
}

td::uint64 ExtMessagePool::NativeInfo::reserved_amount_before(td::uint64 native_nonce,
                                                               td::uint64 before_nonce) const {
  td::uint64 reserved = 0;
  for (const auto &[nonce, message] : messages) {
    if (nonce < native_nonce) {
      continue;
    }
    if (nonce >= before_nonce) {
      break;
    }
    td::uint64 amount = message.amount + message.fee;
    if (amount < message.amount || reserved + amount < reserved) {
      return std::numeric_limits<td::uint64>::max();
    }
    reserved += amount;
  }
  return reserved;
}

size_t ExtMessagePool::CheckedExtMsgCounter::get_msg_count(WorkchainId wc, StdSmcAddress addr) {
  before_query();
  auto it1 = counter_cur_.find({wc, addr});
  auto it2 = counter_prev_.find({wc, addr});
  return (it1 == counter_cur_.end() ? 0 : it1->second) + (it2 == counter_prev_.end() ? 0 : it2->second);
}

size_t ExtMessagePool::CheckedExtMsgCounter::inc_msg_count(WorkchainId wc, StdSmcAddress addr) {
  before_query();
  auto it2 = counter_prev_.find({wc, addr});
  return (it2 == counter_prev_.end() ? 0 : it2->second) + ++counter_cur_[{wc, addr}];
}

void ExtMessagePool::CheckedExtMsgCounter::before_query() {
  while (cleanup_at_.is_in_past()) {
    counter_prev_ = std::move(counter_cur_);
    counter_cur_.clear();
    if (counter_prev_.empty()) {
      cleanup_at_ = td::Timestamp::in(MAX_EXT_MSG_PER_ADDR_TIME_WINDOW / 2.0);
      break;
    }
    cleanup_at_ += MAX_EXT_MSG_PER_ADDR_TIME_WINDOW / 2.0;
  }
}

}  // namespace ton::validator
