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
              reservation_it->second.account_revision != watermark_it->second.revision;
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
    wake_native_callbacks(&committed_source);
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
    auto mc_result = co_await td::actor::await_with_timeout(
                         td::actor::ask(manager_, &ValidatorManager::get_last_liteserver_state_block), deadline)
                         .wrap();
    if (mc_result.is_error()) {
      auto error = mc_result.move_as_error();
      for (const auto &[_, indices] : source_items) {
        for (auto index : indices) {
          reject(index, td::Status::Error(error.code(), error.message().str()));
        }
      }
      source_items.clear();
    } else {
      auto [mc_state, mc_block_id] = mc_result.move_as_ok();
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
          if (deadline && deadline.is_in_past()) {
            for (const auto &address : addresses) {
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error(ErrorCode::timeout,
                                                "external message admission deadline expired"));
              }
            }
            continue;
          }
          ++native_batch_shard_fetches_;
          auto state_result = co_await td::actor::await_with_timeout(
                                  td::actor::ask(manager_, &ValidatorManager::get_block_state_for_litequery,
                                                 shard_block_id),
                                  deadline)
                                  .wrap();
          if (state_result.is_error()) {
            auto error = state_result.move_as_error();
            for (const auto &address : addresses) {
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error(error.code(), error.message().str()));
              }
            }
            continue;
          }
          auto state = state_result.move_as_ok();
          block::gen::ShardStateUnsplit::Record state_info;
          if (!tlb::unpack_cell(state->root_cell(), state_info)) {
            for (const auto &address : addresses) {
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error("cannot unpack pinned shard state header"));
              }
            }
            continue;
          }
          vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(state_info.accounts), 256,
                                           block::tlb::aug_ShardAccounts};
          for (const auto &address : addresses) {
            ++native_batch_account_lookups_;
            block::Account account;
            auto shard_account = accounts.lookup(address.second);
            if (!account.unpack(shard_account, state_info.gen_utime, false)) {
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error("Failed to unpack account state"));
              }
              continue;
            }
            account.block_lt = state_info.gen_lt;
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
            auto &watermark = native_nonce_watermarks_[address];
            auto previous_revision = watermark.revision;
            if (!watermark.observe_account_state(account.native_nonce, available_balance.value(), state_info.gen_lt)) {
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error(
                                  ErrorCode::notready,
                                  "canonical native account state has not caught up with finalized balance"));
              }
              continue;
            }
            if (watermark.revision != previous_revision) {
              changed_native_sources.insert(address);
            }
            auto first_nonce = watermark.first_unconsumed_nonce();
            if (!first_nonce) {
              for (auto index : source_items[address]) {
                reject(index, td::Status::Error("native account nonce space is exhausted"));
              }
              continue;
            }
            source_snapshots[address] = SourceSnapshot{.utime = state_info.gen_utime,
                                                       .lt = state_info.gen_lt,
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
            << " shard_fetches=" << native_batch_shard_fetches_
            << " account_lookups=" << native_batch_account_lookups_ << " accepted=" << native_batch_accepted_
            << " rejected=" << native_batch_rejected_;
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
      }
      return;
    }
    auto first_nonce = watermark_it->second.first_unconsumed_nonce();
    if (!first_nonce) {
      return;
    }
    sources.push_back(SourceState{.source = source,
                                  .info = &info,
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
        source.blocked = true;
        return;
      }
      if (reservation_it->first != source.next_nonce || !reservation_it->second.committed) {
        ++selection.counters.head_gaps;
        source.blocked = true;
        return;
      }
      ++selection.counters.scanned;
      const auto &hash = reservation_it->second.hash;
      auto pool_it = ext_messages_hashes_.find(hash);
      if (pool_it == ext_messages_hashes_.end()) {
        ++selection.counters.head_gaps;
        source.blocked = true;
        return;
      }
      auto priority_it = ext_msgs_.find(pool_it->second.first);
      if (priority_it == ext_msgs_.end()) {
        ++selection.counters.head_gaps;
        source.blocked = true;
        return;
      }
      auto message = priority_it->second.ext_messages_.find(pool_it->second.second);
      if (!message || !message.value()->native_nonce ||
          message.value()->native_nonce.value() != source.next_nonce) {
        ++selection.counters.head_gaps;
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

void ExtMessagePool::enqueue_callback_item(const std::shared_ptr<InstalledCallback> &callback,
                                           std::pair<td::Ref<ExtMessage>, int> item) {
  callback->pending.push_back(std::move(item));
  if (!callback->pump_active) {
    callback->pump_active = true;
    pump_callback(callback).start().detach();
  }
}

td::actor::Task<> ExtMessagePool::pump_callback(std::shared_ptr<InstalledCallback> callback) {
  while (!callback->pending.empty() && callback->callback->cancellation_token.check().is_ok()) {
    auto item = std::move(callback->pending.front());
    callback->pending.pop_front();
    if (!co_await callback->callback->queue.push(std::move(item))) {
      break;
    }
  }
  if (callback->callback->cancellation_token.check().is_error()) {
    callback->pending.clear();
    callback->callback->queue.close();
  }
  callback->pump_active = false;
  if (callback->callback->sync_only) {
    callback->callback->queue.close();
  }
  co_return {};
}

std::size_t ExtMessagePool::fill_callback_native(const std::shared_ptr<InstalledCallback> &callback,
                                                 bool count_install,
                                                 const std::set<NativeAddress> *source_filter) {
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
  auto remaining = delivery_limit - callback->delivered_native.size();
  auto selection = select_native_messages(callback->callback->shard, callback->callback->excluded_messages,
                                          callback->delivered_native, remaining, callback->native_cursor,
                                          source_filter);
  selection.counters.add(counters);
  callback->native_cursor = selection.cursor;
  if (selection.earliest_reactivation) {
    alarm_timestamp().relax(selection.earliest_reactivation);
  }
  auto selected = selection.items.size();
  for (auto &item : selection.items) {
    callback->delivered_native.insert(item.message->hash());
    enqueue_callback_item(callback, {std::move(item.message), item.priority});
  }
  native_queue_counters_.add(selection.counters);
  return selected;
}

std::size_t ExtMessagePool::wake_native_callbacks(const std::set<NativeAddress> *source_filter) {
  std::size_t woken = 0;
  std::erase_if(callbacks_, [&](const std::shared_ptr<InstalledCallback> &callback) {
    if (callback->callback->cancellation_token.check().is_error() ||
        (callback->callback->timeout && callback->callback->timeout.is_in_past())) {
      callback->pending.clear();
      callback->callback->queue.close();
      return true;
    }
    if (fill_callback_native(callback, false, source_filter) != 0) {
      ++woken;
    }
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

  // Native scheduling is deliberately skipped for masterchain installs. Native
  // transfers are basechain-only, and copying/scanning their large treaps while
  // producing a masterchain anchor created avoidable multi-second stalls.
  installed->native_cursor = native_scheduler_cursor_;
  fill_callback_native(installed, true);
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
      enqueue_callback_item(installed, {msg->message, it->first});
    }
    if (installed->generic_selected >= generic_limit) {
      break;
    }
  }

  VLOG(VALIDATOR_DEBUG) << "install_collator_queue: selected_native=" << installed->delivered_native.size()
                        << " selected_generic=" << installed->generic_selected
                        << " excluded=" << installed->callback->excluded_messages.size()
                        << " native_limit=" << native_collator_queue_limit_ << " shard=" << shard;
  if (!installed->pump_active) {
    installed->pump_active = true;
    pump_callback(installed).start().detach();
  }
  if (!installed->callback->sync_only) {
    alarm_timestamp().relax(installed->callback->timeout);
    callbacks_.push_back(std::move(installed));
  }
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

void ExtMessagePool::finalize_native_external_messages(
    std::vector<FinalizedNativeExternalMessage> messages) {
  using Address = std::pair<WorkchainId, StdSmcAddress>;
  std::map<Address, td::uint64> finalized_nonce;
  std::set<std::pair<int, MessageId>> to_erase;
  for (const auto &message : messages) {
    Address address{message.workchain, message.source};
    auto [it, inserted] = finalized_nonce.emplace(address, message.nonce);
    if (!inserted) {
      it->second = std::max(it->second, message.nonce);
    }
    auto exact_it = ext_messages_hashes_.find(message.hash);
    if (exact_it != ext_messages_hashes_.end()) {
      to_erase.insert(exact_it->second);
    }
  }

  // Publish this monotonic watermark before touching pool indexes or
  // reservations. Signature verification is asynchronous: a coroutine that
  // fetched the account before finalization must observe this value when it
  // resumes, even if cleanup removes the last NativeInfo entry.
  for (const auto &[address, max_nonce] : finalized_nonce) {
    auto &watermark = native_nonce_watermarks_[address];
    watermark.observe_finalized_nonce(max_nonce);
    CHECK(watermark.is_consumed(max_nonce));
  }

  // A finalized nonce also makes a locally admitted re-signed variant
  // obsolete even when this validator never held the winning hash. NativeInfo
  // owns exactly one admitted hash per nonce, so walk only its finalized
  // prefix. This avoids sources x per-address-backlog persistent-tree lookups.
  for (const auto &[address, max_nonce] : finalized_nonce) {
    auto account_it = native_accounts_.find(address);
    if (account_it == native_accounts_.end()) {
      continue;
    }
    for (auto it = account_it->second.messages.begin();
         it != account_it->second.messages.end() && it->first <= max_nonce; ++it) {
      auto pool_it = ext_messages_hashes_.find(it->second.hash);
      if (pool_it != ext_messages_hashes_.end()) {
        to_erase.insert(pool_it->second);
      }
    }
  }
  for (const auto &[priority, id] : to_erase) {
    erase_message(priority, id);
  }

  // Also fail reservations whose signature check completed but whose outer
  // insertion had not yet reached the persistent mempool indexes.
  for (const auto &[address, max_nonce] : finalized_nonce) {
    auto account_it = native_accounts_.find(address);
    if (account_it == native_accounts_.end()) {
      continue;
    }
    for (auto it = account_it->second.messages.begin(); it != account_it->second.messages.end() &&
                                                        it->first <= max_nonce;) {
      if (it->second.allow_broadcast_promise) {
        it->second.allow_broadcast_promise.set_error(
            td::Status::Error("native nonce was already consumed by a finalized block"));
      }
      it->second.insertion_failed("native nonce was already consumed by a finalized block");
      it = account_it->second.messages.erase(it);
    }
    if (account_it->second.messages.empty()) {
      native_accounts_.erase(account_it);
    }
  }
  LOG(INFO) << "finalized native external cleanup: candidates=" << messages.size()
            << " sources=" << finalized_nonce.size() << " pool_entries=" << to_erase.size();
  if (!finalized_nonce.empty()) {
    std::set<NativeAddress> finalized_sources;
    for (const auto &[address, _] : finalized_nonce) {
      finalized_sources.insert(address);
    }
    wake_native_callbacks(&finalized_sources);
  }
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

bool ExtMessagePool::erase_message(int priority, const MessageId &id) {
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
  if (native_nonce) {
    msgs.native_messages_ =
        msgs.native_messages_.erase(NativeMessageId{native_nonce.value(), id.dst, id.hash});
    auto native_it = native_accounts_.find(address);
    if (native_it != native_accounts_.end()) {
      auto reservation_it = native_it->second.messages.find(native_nonce.value());
      if (reservation_it != native_it->second.messages.end()) {
        reservation_it->second.insertion_failed("native message was removed from the mempool");
        native_it->second.messages.erase(reservation_it);
      }
      if (native_it->second.messages.empty()) {
        native_accounts_.erase(native_it);
      }
    }
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
  vec.emplace_back("total.ext_msg_mempool", PSTRING() << "messages:" << mempool_total << " active:" << mempool_active
                                                      << " native:" << mempool_native
                                                      << " priorities:" << ext_msgs_.size());
  vec.emplace_back("total.ext_msg_native_pending", PSTRING() << "accounts:" << native_accounts_.size()
                                                             << " messages:" << native_pending
                                                             << " nonce_watermarks:"
                                                             << native_nonce_watermarks_.size());
  vec.emplace_back("total.ext_msg_native_config", PSTRING() << "collator_queue_limit:"
                                                            << native_collator_queue_limit_ << " max_retention_s:"
                                                            << native_mempool_max_ttl_);
  vec.emplace_back("total.ext_msg_batch_admission",
                   PSTRING() << "batches:" << native_batch_count_ << " messages:" << native_batch_messages_
                             << " unique:" << native_batch_unique_messages_
                             << " shard_fetches:" << native_batch_shard_fetches_
                             << " account_lookups:" << native_batch_account_lookups_
                             << " accepted:" << native_batch_accepted_ << " rejected:" << native_batch_rejected_);
  vec.emplace_back(
      "total.ext_msg_native_scheduler",
      PSTRING() << "installs:" << native_queue_counters_.installs
                << " masterchain_installs:" << native_queue_counters_.masterchain_installs
                << " scanned:" << native_queue_counters_.scanned << " selected:" << native_queue_counters_.selected
                << " active:" << native_queue_counters_.active << " inactive:" << native_queue_counters_.inactive
                << " excluded:" << native_queue_counters_.excluded << " expired:" << native_queue_counters_.expired
                << " delivered_skips:" << native_queue_counters_.already_delivered
                << " ready_sources:" << native_queue_counters_.ready_sources
                << " head_gaps:" << native_queue_counters_.head_gaps << " runs:" << native_queue_counters_.runs
                << " run_messages:" << native_queue_counters_.run_messages
                << " max_run_size:" << native_queue_counters_.max_run_size
                << " delayed:" << native_queue_counters_.delayed
                << " reactivated:" << native_queue_counters_.reactivated
                << " reactivation_wakes:" << native_queue_counters_.reactivation_wakes);
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
      callback->pending.clear();
      callback->callback->queue.close();
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
        return true;
      }
      if (shard_contains(callback->callback->shard, message->shard()) &&
          !std::binary_search(callback->callback->excluded_messages.begin(),
                              callback->callback->excluded_messages.end(), message->hash()) &&
          (callback->callback->shard.workchain != masterchainId ||
           callback->generic_selected < STANDARD_COLLATOR_QUEUE_LIMIT)) {
        ++callback->generic_selected;
        enqueue_callback_item(callback, std::make_pair(message, priority));
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
    CHECK(native_it->second.commit_message(native_transfer->nonce));
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
      auto &initial_watermark = native_nonce_watermarks_[native_address];
      auto previous_revision = initial_watermark.revision;
      if (!initial_watermark.observe_account_state(acc.native_nonce, available_balance.value(), lt)) {
        co_return td::Status::Error(ErrorCode::notready,
                                    "canonical native account state has not caught up with finalized balance");
      }
      initial_native_nonce = initial_watermark.first_unconsumed_nonce();
      account_revision = initial_watermark.revision;
      if (account_revision != previous_revision) {
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
    auto obsolete_hashes = existing_native_info->second.process_messages(current_native_nonce.value(), utime);
    for (const auto &hash : obsolete_hashes) {
      auto pool_it = ext_messages_hashes_.find(hash);
      if (pool_it != ext_messages_hashes_.end()) {
        erase_message(pool_it->second.first, pool_it->second.second);
      }
    }
    auto cleanup_it = native_accounts_.find(native_address);
    if (cleanup_it != native_accounts_.end() && cleanup_it->second.messages.empty()) {
      native_accounts_.erase(cleanup_it);
    }
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
        }
        co_return td::Status::Error("native transfer pending amount overflow");
      }
      if (reserved_amount + required_amount > available_balance) {
        if (native_info.messages.empty()) {
          native_accounts_.erase(native_address);
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

std::vector<ExtMessage::Hash> ExtMessagePool::NativeInfo::process_messages(td::uint64 native_nonce,
                                                                           UnixTime utime) {
  std::vector<ExtMessage::Hash> obsolete_hashes;
  observed_nonce = std::max(observed_nonce, native_nonce);
  observed_utime = std::max(observed_utime, utime);
  native_nonce = observed_nonce;
  utime = observed_utime;
  for (auto it = messages.begin(); it != messages.end();) {
    auto &[nonce, message] = *it;
    if (nonce < native_nonce) {
      obsolete_hashes.push_back(message.hash);
      if (message.allow_broadcast_promise) {
        message.allow_broadcast_promise.set_error(
            td::Status::Error(PSTRING() << "Too old native nonce: msg_nonce=" << nonce
                                        << ", account_nonce=" << native_nonce));
      }
      message.insertion_failed("native message nonce became obsolete before insertion");
      it = messages.erase(it);
      continue;
    }
    if (message.valid_until <= utime) {
      obsolete_hashes.push_back(message.hash);
      if (message.allow_broadcast_promise) {
        message.allow_broadcast_promise.set_error(td::Status::Error("native transfer valid_until is in the past"));
      }
      message.insertion_failed("native transfer expired before insertion");
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
  return obsolete_hashes;
}

bool ExtMessagePool::NativeInfo::commit_message(td::uint64 native_nonce) {
  auto it = messages.find(native_nonce);
  if (it == messages.end()) {
    return false;
  }
  it->second.committed = true;
  it->second.insertion_succeeded();
  process_messages(observed_nonce, observed_utime);
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
