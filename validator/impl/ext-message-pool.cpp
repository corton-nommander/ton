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
  LOG(INFO) << "started " << workers << " native signature verifier workers";
}

td::actor::Task<ExtMessagePool::CheckResult> ExtMessagePool::check_add_external_message(td::BufferSlice data,
                                                                                        int priority,
                                                                                        bool add_to_mempool) {
  if (last_masterchain_state_.is_null()) {
    co_return td::Status::Error(ErrorCode::notready, "not ready");
  }
  auto message = co_await create_ext_message(std::move(data), last_masterchain_state_->get_ext_msg_limits());
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  if (add_to_mempool) {
    auto existing = ext_messages_hashes_.find(message->hash());
    if (existing != ext_messages_hashes_.end() && existing->second.first >= priority) {
      // sendMessage is idempotent for a byte-identical message that is still in
      // the mempool.  In particular, a client can safely retry after losing the
      // first response without repeating the expensive account/signature check.
      auto [wait_allow_broadcast, allow_broadcast_promise] = td::actor::StartedTask<>::make_bridge();
      allow_broadcast_promise.set_value(td::Unit{});
      co_return CheckResult{.message = std::move(message),
                            .wait_allow_broadcast = std::move(wait_allow_broadcast),
                            .should_broadcast = false};
    }
  }
  if (checked_ext_msg_counter_.get_msg_count(wc, addr) >= MAX_EXT_MSG_PER_ADDR) {
    co_return td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex());
  }
  td::optional<td::uint32> msg_seqno;
  auto result = co_await check_message(message, msg_seqno).wrap();
  ++(result.is_ok() ? total_check_ext_messages_ok_ : total_check_ext_messages_error_);
  if (result.is_error()) {
    co_return result.move_as_error();
  }
  if (checked_ext_msg_counter_.inc_msg_count(wc, addr) > MAX_EXT_MSG_PER_ADDR) {
    rollback_checked_message(message, msg_seqno);
    co_return td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex());
  }
  if (add_to_mempool) {
    auto add_status = add_message_to_mempool(message, priority, msg_seqno);
    if (add_status.is_error()) {
      // check_message reserves wallet seqnos/native nonces before insertion so
      // concurrent checks cannot overspend.  A rejected insertion must release
      // that reservation; otherwise later valid messages are rejected as
      // duplicates even though this message was never available to a collator.
      rollback_checked_message(message, msg_seqno);
      co_return std::move(add_status);
    }
  }
  auto commit_status = commit_checked_message(message, msg_seqno);
  if (commit_status.is_error()) {
    if (add_to_mempool) {
      erase_message(priority, MessageId{message->shard(), message->hash()});
    }
    rollback_checked_message(message, msg_seqno);
    co_return std::move(commit_status);
  }
  co_return result.move_as_ok();
}

void ExtMessagePool::install_collator_queue(ShardIdFull shard, std::unique_ptr<ExtMsgCallback> callback) {
  // Compute shard key range [lo, hi) for splitting
  td::uint64 lo_prefix = shard.shard & (shard.shard - 1);
  td::uint64 hi_prefix_plus1 = (shard.shard | (shard.shard - 1)) + 1;  // may overflow to 0
  MessageId shard_lo{AccountIdPrefixFull{shard.workchain, lo_prefix}, Bits256::zero()};
  MessageId shard_hi{AccountIdPrefixFull{hi_prefix_plus1 == 0 ? shard.workchain + 1 : shard.workchain, hi_prefix_plus1},
                     Bits256::zero()};

  // Take O(log n) generic shard slices and O(1) native snapshots from each
  // priority level.  Native messages have their own nonce-first index so a
  // large backlog does not have to be rebuilt and sorted for every candidate.
  using Treap = td::PersistentTreap<MessageId, std::shared_ptr<MempoolMsg>>;
  using Snapshot = std::vector<std::pair<int, Treap>>;
  using NativeTreap = td::PersistentTreap<NativeMessageId, std::shared_ptr<MempoolMsg>>;
  using NativeSnapshot = std::vector<std::pair<int, NativeTreap>>;
  Snapshot generic_snapshot;
  NativeSnapshot native_snapshot;
  for (auto it = ext_msgs_.rbegin(); it != ext_msgs_.rend(); ++it) {
    auto [_, in_shard, __] = it->second.generic_messages_.split_range(shard_lo, shard_hi);
    if (!in_shard.empty()) {
      generic_snapshot.emplace_back(it->first, std::move(in_shard));
    }
    if (!it->second.native_messages_.empty()) {
      native_snapshot.emplace_back(it->first, it->second.native_messages_);
    }
  }

  // Spawn a coroutine that drains native messages by source nonce and leaves other messages in stable key order.
  auto push_existing = [](ExtMsgQueue queue, td::CancellationToken token, ShardIdFull shard,
                          Snapshot generic_snapshot, NativeSnapshot native_snapshot,
                          bool sync_only) -> td::actor::Task<> {
    struct QueueItem {
      int priority;
      MessageId key;
      std::shared_ptr<MempoolMsg> msg;
    };
    auto comes_before = [](const QueueItem &a, const QueueItem &b) {
      if (a.priority != b.priority) {
        return a.priority > b.priority;
      }
      bool a_native = static_cast<bool>(a.msg->native_nonce);
      bool b_native = static_cast<bool>(b.msg->native_nonce);
      if (a_native != b_native) {
        return a_native;
      }
      if (a_native && b_native) {
        if (a.msg->native_nonce.value() != b.msg->native_nonce.value()) {
          return a.msg->native_nonce.value() < b.msg->native_nonce.value();
        }
        auto a_address = a.msg->address();
        auto b_address = b.msg->address();
        if (a_address < b_address) {
          return true;
        }
        if (b_address < a_address) {
          return false;
        }
      }
      return a.key < b.key;
    };
    SCOPE_EXIT {
      if (sync_only) {
        queue.close();
      }
    };
    td::Timer t;
    std::vector<QueueItem> items;
    items.reserve(NATIVE_COLLATOR_QUEUE_LIMIT);
    size_t native_selected = 0;
    for (auto &[priority, treap] : native_snapshot) {
      for (size_t index = 0; index < treap.size() && native_selected < NATIVE_COLLATOR_QUEUE_LIMIT; ++index) {
        if (token.check().is_error()) {
          co_return {};
        }
        auto [native_key, msg] = treap.at(index);
        if (!shard_contains(shard, msg->message->shard()) || msg->expired() || !msg->is_active()) {
          continue;
        }
        items.push_back(QueueItem{.priority = priority,
                                  .key = MessageId{native_key.dst, native_key.hash},
                                  .msg = std::move(msg)});
        ++native_selected;
      }
      if (native_selected >= NATIVE_COLLATOR_QUEUE_LIMIT) {
        break;
      }
    }
    for (auto &[priority, treap] : generic_snapshot) {
      // The treap is already an immutable snapshot.  Erasing rank zero on
      // every iteration rebuilt O(log n) persistent paths and allocated a
      // new temporary treap for every mempool message.  Under a 100k+ native
      // backlog that consumed the complete collation deadline before the
      // first queue item was delivered.  Ranked reads preserve the snapshot
      // and avoid all of those transient allocations.
      auto size = treap.size();
      for (size_t index = 0; index < size; ++index) {
        if (token.check().is_error()) {
          co_return {};
        }
        auto [key, msg] = treap.at(index);
        if (msg->expired() || !msg->is_active()) {
          continue;
        }
        items.push_back(QueueItem{.priority = priority, .key = key, .msg = std::move(msg)});
      }
    }
    std::sort(items.begin(), items.end(), comes_before);
    size_t pushed = 0;
    for (auto &item : items) {
      if (token.check().is_error()) {
        co_return {};
      }
      bool ok = co_await queue.push(std::make_pair(item.msg->message, item.priority));
      if (!ok) {
        co_return {};
      }
      ++pushed;
    }
    LOG(WARNING) << "install_collator_queue: selected_native=" << native_selected << " pushed=" << pushed
                 << " existing messages to shard " << shard << " in " << t.elapsed() << "s";
    co_return {};
  };
  push_existing(callback->queue, callback->cancellation_token, shard, std::move(generic_snapshot),
                std::move(native_snapshot), callback->sync_only)
      .start()
      .detach();

  if (!callback->sync_only) {
    alarm_timestamp().relax(callback->timeout);
    callbacks_.push_back(std::move(callback));
  }
}

void ExtMessagePool::cleanup_external_messages(ShardIdFull shard) {
  // Clean up expired messages
  for (auto &[priority, msgs] : ext_msgs_) {
    std::vector<MessageId> to_erase;
    for (size_t i = 0; i < msgs.ext_messages_.size(); i++) {
      auto [key, msg] = msgs.ext_messages_.at(i);
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
        msg_opt.value()->postpone_native();
      } else if (msg_opt && msgs.ext_messages_.size() < SOFT_MEMPOOL_LIMIT && msg_opt.value()->can_postpone()) {
        msg_opt.value()->postpone();
      } else {
        erase_message(priority, msg_id);
      }
    }
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
      native_it->second.messages.erase(native_nonce.value());
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
    for (size_t i = 0; i < msgs.ext_messages_.size(); ++i) {
      auto [__, msg] = msgs.ext_messages_.at(i);
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
                                                             << " messages:" << native_pending);
  return vec;
}

void ExtMessagePool::alarm() {
  if (cleanup_mempool_at_.is_in_past()) {
    cleanup_external_messages(ShardIdFull{masterchainId, shardIdAll});
    cleanup_external_messages(ShardIdFull{basechainId, shardIdAll});
    cleanup_mempool_at_ = td::Timestamp::in(250.0);
  }
  alarm_timestamp().relax(cleanup_mempool_at_);
  std::erase_if(callbacks_, [&](const std::unique_ptr<ExtMsgCallback> &callback) -> bool {
    if (callback->timeout && callback->timeout.is_in_past()) {
      return true;
    }
    alarm_timestamp().relax(callback->timeout);
    return false;
  });
}

td::Status ExtMessagePool::add_message_to_mempool(td::Ref<ExtMessage> message, int priority,
                                                  td::optional<td::uint32> msg_seqno) {
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  auto &msgs = ext_msgs_[priority];
  auto msg = std::make_shared<MempoolMsg>(message);
  msg->msg_seqno = msg_seqno;
  auto native_transfer_res = block::NativeTransfer::unpack_external(message->root_cell());
  if (native_transfer_res.is_ok()) {
    msg->native_nonce = native_transfer_res.move_as_ok().nonce;
  }
  MessageId id{message->shard(), message->hash()};
  auto address = msg->address();
  auto it2 = ext_messages_hashes_.find(id.hash);
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
  LOG(INFO) << "adding message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority << " to mempool";
  std::erase_if(callbacks_, [&](const std::unique_ptr<ExtMsgCallback> &callback) -> bool {
    if (callback->cancellation_token.check().is_error()) {
      return true;
    }
    if (shard_contains(callback->shard, message->shard())) {
      callback->queue.try_push(std::make_pair(message, priority)).detach();
    }
    return false;
  });
  return td::Status::OK();
}

td::Status ExtMessagePool::commit_checked_message(td::Ref<ExtMessage> message,
                                                  td::optional<td::uint32> msg_seqno) {
  auto address = std::make_pair(message->wc(), message->addr());
  auto native_transfer = block::NativeTransfer::unpack_external(message->root_cell());
  if (native_transfer.is_ok()) {
    auto native_it = native_accounts_.find(address);
    if (native_it == native_accounts_.end() || !native_it->second.commit_message(native_transfer.ok().nonce)) {
      return td::Status::Error("native message reservation disappeared before mempool commit");
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
                                              td::optional<td::uint32> msg_seqno) {
  auto address = std::make_pair(message->wc(), message->addr());
  auto native_transfer = block::NativeTransfer::unpack_external(message->root_cell());
  if (native_transfer.is_ok()) {
    auto native_it = native_accounts_.find(address);
    if (native_it != native_accounts_.end()) {
      auto message_it = native_it->second.messages.find(native_transfer.ok().nonce);
      if (message_it != native_it->second.messages.end()) {
        if (message_it->second.allow_broadcast_promise) {
          message_it->second.allow_broadcast_promise.set_error(
              td::Status::Error("native message was not inserted into the mempool"));
        }
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
                                                                           td::optional<td::uint32> &msg_seqno) {
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  auto [shard_acc, utime, lt, config] = co_await run_fetch_account_state(wc, addr, manager_);
  bool special = wc == masterchainId && config->is_special_smartcontract(addr);
  block::Account acc;
  if (!acc.unpack(shard_acc, utime, special)) {
    co_return td::Status::Error(PSLICE() << "Failed to unpack account state");
  }
  acc.block_lt = lt;

  auto [wait_allow_broadcast, allow_broadcast_promise] = td::actor::StartedTask<>::make_bridge();
  CheckResult check_result{.message = message, .wait_allow_broadcast = std::move(wait_allow_broadcast)};

  auto native_transfer_res = block::NativeTransfer::unpack_external(message->root_cell());
  if (native_transfer_res.is_ok()) {
    auto transfer = native_transfer_res.move_as_ok();
    if (wc != basechainId || transfer.src != addr) {
      co_return td::Status::Error("native transfer is routed to the wrong source account");
    }
    if (transfer.valid_until <= (UnixTime)td::Clocks::system()) {
      co_return td::Status::Error("native transfer valid_until is in the past");
    }
    if (transfer.nonce < acc.native_nonce) {
      co_return td::Status::Error(PSTRING() << "Too old native nonce: msg_nonce=" << transfer.nonce
                                            << ", account_nonce=" << acc.native_nonce);
    }
    if (transfer.nonce - acc.native_nonce > MAX_NATIVE_NONCE_DIFF) {
      co_return td::Status::Error(PSTRING() << "Too new native nonce: msg_nonce=" << transfer.nonce
                                            << ", account_nonce=" << acc.native_nonce);
    }
    if (acc.status != block::Account::acc_uninit || !acc.is_native) {
      co_return td::Status::Error("native transfer source account must be balance-only");
    }
    if (acc.balance.extra.not_null() || acc.balance.grams.is_null() || !acc.balance.grams->unsigned_fits_bits(64)) {
      co_return td::Status::Error("native transfer source balance must be uint64 grams without extra currencies");
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
    auto signature_result = co_await td::actor::ask(verifier, &NativeSignatureVerifier::verify, transfer).wrap();
    if (signature_result.is_error()) {
      co_return signature_result.move_as_error();
    }

    // Do not retain references into native_accounts_ across the asynchronous
    // signature check. Other checks for the same source can complete while this
    // coroutine is suspended and may erase or replace that map entry.
    auto &native_info = native_accounts_[{wc, addr}];
    SCOPE_EXIT {
      if (native_info.messages.empty()) {
        native_accounts_.erase({wc, addr});
      }
    };
    native_info.process_messages(acc.native_nonce, utime);
    if (native_info.messages.contains(transfer.nonce)) {
      co_return td::Status::Error(PSTRING() << "Duplicate native nonce " << transfer.nonce);
    }
    td::uint64 reserved_amount = native_info.reserved_amount(acc.native_nonce);
    if (reserved_amount + required_amount < reserved_amount) {
      co_return td::Status::Error("native transfer pending amount overflow");
    }
    required = block::CurrencyCollection{td::make_refint(reserved_amount + required_amount)};
    if (!(acc.balance >= required)) {
      co_return td::Status::Error("native transfer has insufficient source balance");
    }
    auto insert_result = native_info.messages.emplace(
        transfer.nonce,
        NativeMessageInfo{.amount = transfer.amount,
                          .fee = transfer.fee,
                          .valid_until = transfer.valid_until,
                          .allow_broadcast_promise = std::move(allow_broadcast_promise),
                          .committed = false});
    if (!insert_result.second) {
      co_return td::Status::Error(PSTRING() << "Duplicate native nonce " << transfer.nonce);
    }
    co_return check_result;
  }

  const WalletMessageProcessor *wallet =
      acc.code.not_null() ? WalletMessageProcessor::get(acc.code->get_hash().bits()) : nullptr;
  if (wallet != nullptr) {
    msg_seqno = co_await check_message_to_wallet(message, wallet, std::move(acc), utime, lt, std::move(config),
                                                 std::move(allow_broadcast_promise));
    co_return check_result;
  }
  wallets_.erase({wc, addr});
  co_await ExtMessageQ::run_message_on_account(wc, &acc, utime, lt + 1, message->root_cell(), std::move(config));
  allow_broadcast_promise.set_value(td::Unit{});
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

void ExtMessagePool::NativeInfo::process_messages(td::uint64 native_nonce, UnixTime utime) {
  observed_nonce = std::max(observed_nonce, native_nonce);
  observed_utime = std::max(observed_utime, utime);
  native_nonce = observed_nonce;
  utime = observed_utime;
  for (auto it = messages.begin(); it != messages.end();) {
    auto &[nonce, message] = *it;
    if (nonce < native_nonce) {
      if (message.allow_broadcast_promise) {
        message.allow_broadcast_promise.set_error(
            td::Status::Error(PSTRING() << "Too old native nonce: msg_nonce=" << nonce
                                        << ", account_nonce=" << native_nonce));
      }
      it = messages.erase(it);
      continue;
    }
    if (message.valid_until <= utime) {
      if (message.allow_broadcast_promise) {
        message.allow_broadcast_promise.set_error(td::Status::Error("native transfer valid_until is in the past"));
      }
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
}

bool ExtMessagePool::NativeInfo::commit_message(td::uint64 native_nonce) {
  auto it = messages.find(native_nonce);
  if (it == messages.end()) {
    return false;
  }
  it->second.committed = true;
  process_messages(observed_nonce, observed_utime);
  return true;
}

td::uint64 ExtMessagePool::NativeInfo::reserved_amount(td::uint64 native_nonce) const {
  td::uint64 reserved = 0;
  for (const auto &[nonce, message] : messages) {
    if (nonce < native_nonce) {
      continue;
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
