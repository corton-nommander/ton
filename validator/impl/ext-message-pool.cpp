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
  auto deadline_status = [&]() {
    return deadline && deadline.is_in_past()
               ? td::Status::Error(ErrorCode::timeout,
                                   "external message admission deadline expired")
               : td::Status::OK();
  };
  auto initial_deadline_status = deadline_status();
  if (initial_deadline_status.is_error()) {
    co_return std::move(initial_deadline_status);
  }
  if (last_masterchain_state_.is_null()) {
    co_return td::Status::Error(ErrorCode::notready, "not ready");
  }
  auto message = co_await create_ext_message(std::move(data), last_masterchain_state_->get_ext_msg_limits());
  auto parsed_deadline_status = deadline_status();
  if (parsed_deadline_status.is_error()) {
    co_return std::move(parsed_deadline_status);
  }
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
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
      co_return CheckResult{.message = std::move(message),
                            .wait_allow_broadcast = std::move(wait_allow_broadcast),
                            .should_broadcast = false};
    }
  }
  if (checked_ext_msg_counter_.get_msg_count(wc, addr) >= MAX_EXT_MSG_PER_ADDR) {
    co_return td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex());
  }
  td::optional<td::uint32> msg_seqno;
  auto result = co_await check_message(message, msg_seqno, deadline).wrap();
  if (result.is_error()) {
    ++total_check_ext_messages_error_;
    co_return result.move_as_error();
  }
  if (!result.ok_ref().should_broadcast) {
    // check_message may have suspended for native signature verification.  A
    // byte-identical retry can be inserted by another coroutine while this
    // one is suspended; treat that race exactly like the fast-path duplicate
    // check above and, importantly, do not reserve/add/rollback its nonce.
    co_return result.move_as_ok();
  }
  ++total_check_ext_messages_ok_;
  auto before_commit_status = deadline_status();
  if (before_commit_status.is_error()) {
    rollback_checked_message(message, msg_seqno);
    co_return std::move(before_commit_status);
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
  std::sort(callback->excluded_messages.begin(), callback->excluded_messages.end());
  callback->excluded_messages.erase(
      std::unique(callback->excluded_messages.begin(), callback->excluded_messages.end()),
      callback->excluded_messages.end());
  auto excluded_snapshot = callback->excluded_messages;

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
                          bool sync_only, std::size_t native_queue_limit,
                          std::vector<ExtMessage::Hash> excluded_messages) -> td::actor::Task<> {
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
    size_t native_selected = 0;
    size_t pushed = 0;
    if (generic_snapshot.empty()) {
      // NativeTreap already has the required nonce/address/hash order and the
      // priority snapshots were captured descending. Stream directly into the
      // bounded queue so an idle work-trigger sees its first item immediately;
      // materializing the full 262k snapshot before the first push could itself
      // exceed the trigger deadline.
      for (auto &[priority, treap] : native_snapshot) {
        auto iterator = treap.in_order();
        while (native_selected < native_queue_limit) {
          if (token.check().is_error()) {
            co_return {};
          }
          auto item = iterator.next();
          if (!item) {
            break;
          }
          auto [native_key, msg] = std::move(item.value());
          if (!shard_contains(shard, msg->message->shard()) || msg->expired() || !msg->is_active() ||
              std::binary_search(excluded_messages.begin(), excluded_messages.end(), msg->message->hash())) {
            continue;
          }
          ++native_selected;
          if (!co_await queue.push(std::make_pair(msg->message, priority))) {
            co_return {};
          }
          ++pushed;
        }
        if (native_selected >= native_queue_limit) {
          break;
        }
      }
      LOG(WARNING) << "install_collator_queue: selected_native=" << native_selected << " pushed=" << pushed
                   << " excluded=" << excluded_messages.size() << " limit=" << native_queue_limit
                   << " existing native messages to shard " << shard << " in " << t.elapsed() << "s";
      co_return {};
    }

    std::vector<QueueItem> items;
    items.reserve(native_queue_limit);
    for (auto &[priority, treap] : native_snapshot) {
      auto iterator = treap.in_order();
      while (native_selected < native_queue_limit) {
        if (token.check().is_error()) {
          co_return {};
        }
        auto item = iterator.next();
        if (!item) {
          break;
        }
        auto [native_key, msg] = std::move(item.value());
        if (!shard_contains(shard, msg->message->shard()) || msg->expired() || !msg->is_active()) {
          continue;
        }
        if (std::binary_search(excluded_messages.begin(), excluded_messages.end(), msg->message->hash())) {
          continue;
        }
        items.push_back(QueueItem{.priority = priority,
                                  .key = MessageId{native_key.dst, native_key.hash},
                                  .msg = std::move(msg)});
        ++native_selected;
      }
      if (native_selected >= native_queue_limit) {
        break;
      }
    }
    for (auto &[priority, treap] : generic_snapshot) {
      auto iterator = treap.in_order();
      while (auto item = iterator.next()) {
        if (token.check().is_error()) {
          co_return {};
        }
        auto [key, msg] = std::move(item.value());
        if (msg->expired() || !msg->is_active()) {
          continue;
        }
        if (std::binary_search(excluded_messages.begin(), excluded_messages.end(), msg->message->hash())) {
          continue;
        }
        items.push_back(QueueItem{.priority = priority, .key = key, .msg = std::move(msg)});
      }
    }
    std::sort(items.begin(), items.end(), comes_before);
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
                 << " excluded=" << excluded_messages.size() << " limit=" << native_queue_limit
                 << " existing messages to shard " << shard << " in " << t.elapsed() << "s";
    co_return {};
  };
  push_existing(callback->queue, callback->cancellation_token, shard, std::move(generic_snapshot),
                std::move(native_snapshot), callback->sync_only, native_collator_queue_limit_,
                std::move(excluded_snapshot))
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
        msg_opt.value()->postpone_native();
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
  auto address = std::make_pair(wc, addr);
  auto &msgs = ext_msgs_[priority];
  auto msg = std::make_shared<MempoolMsg>(message);
  msg->msg_seqno = msg_seqno;
  auto native_transfer_res = block::NativeTransfer::unpack_external(message->root_cell());
  if (native_transfer_res.is_ok()) {
    auto transfer = native_transfer_res.move_as_ok();
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
  LOG(INFO) << "adding message addr=" << wc << ":" << addr.to_hex() << " prio=" << priority << " to mempool";
  std::erase_if(callbacks_, [&](const std::unique_ptr<ExtMsgCallback> &callback) -> bool {
    if (callback->cancellation_token.check().is_error()) {
      return true;
    }
    if (shard_contains(callback->shard, message->shard()) &&
        !std::binary_search(callback->excluded_messages.begin(), callback->excluded_messages.end(),
                            message->hash())) {
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
    auto watermark_it = native_nonce_watermarks_.find(address);
    if (watermark_it != native_nonce_watermarks_.end() &&
        watermark_it->second.is_consumed(native_transfer.ok().nonce)) {
      return td::Status::Error(PSTRING() << "native nonce " << native_transfer.ok().nonce
                                         << " was consumed before mempool commit");
    }
    auto native_it = native_accounts_.find(address);
    if (native_it == native_accounts_.end()) {
      return td::Status::Error("native message reservation disappeared before mempool commit");
    }
    auto reservation_it = native_it->second.messages.find(native_transfer.ok().nonce);
    if (reservation_it == native_it->second.messages.end()) {
      return td::Status::Error("native message reservation disappeared before mempool commit");
    }
    if (watermark_it == native_nonce_watermarks_.end() ||
        reservation_it->second.account_revision != watermark_it->second.revision) {
      return td::Status::Error(ErrorCode::notready,
                               "native account changed before mempool commit; retry admission");
    }
    CHECK(native_it->second.commit_message(native_transfer.ok().nonce));
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
                                                                           td::optional<td::uint32> &msg_seqno,
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
      if (!initial_watermark.observe_account_state(acc.native_nonce, available_balance.value(), lt)) {
        co_return td::Status::Error(ErrorCode::notready,
                                    "canonical native account state has not caught up with finalized balance");
      }
      initial_native_nonce = initial_watermark.first_unconsumed_nonce();
      account_revision = initial_watermark.revision;
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

    // Do not retain references into native_accounts_ across the asynchronous
    // signature check. Other checks for the same source can complete while this
    // coroutine is suspended and may erase or replace that map entry.
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
                                            << transfer.nonce << ", account_nonce="
                                            << current_native_nonce.value());
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

        // Two byte-identical checks can both finish signature verification
        // before the owner reaches add_message_to_mempool(). Join the owner's
        // insertion outcome rather than claiming success early: if its later
        // mempool insertion fails, every identical retry receives that error.
        auto [waiter, waiter_promise] = td::actor::StartedTask<>::make_bridge();
        pending_it->second.insertion_waiters.emplace_back(std::move(waiter_promise));
        wait_for_insertion = std::move(waiter);
      } else {
        // Funds are reserved in nonce order. A lower nonce always has priority
        // over already-pending higher nonces, regardless of the order in which
        // asynchronous signature workers completed.
        td::uint64 reserved_amount =
            native_info.reserved_amount_before(current_native_nonce.value(), transfer.nonce);
        if (reserved_amount + required_amount < reserved_amount) {
          if (native_info.messages.empty()) {
            native_accounts_.erase(native_address);
          }
          co_return td::Status::Error("native transfer pending amount overflow");
        }
        required = block::CurrencyCollection{td::make_refint(reserved_amount + required_amount)};
        if (!(acc.balance >= required)) {
          if (native_info.messages.empty()) {
            native_accounts_.erase(native_address);
          }
          co_return td::Status::Error("native transfer has insufficient source balance");
        }
        // NativeMessageInfo has a destructor that resolves joined insertion
        // waiters, so construct it in place instead of relying on an implicit
        // move constructor suppressed by that destructor.
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

        // A newly arrived lower nonce can make previously reserved higher
        // nonces unaffordable. Keep the nonce-ordered prefix and discard the
        // first unaffordable transfer and its entire tail; otherwise that tail
        // can never execute and would strand the source until TTL expiry.
        std::vector<std::pair<td::uint64, ExtMessage::Hash>> unaffordable_tail;
        td::uint64 prefix_amount = 0;
        bool tail_started = false;
        for (const auto &[nonce, pending] : native_info.messages) {
          if (nonce < current_native_nonce.value()) {
            continue;
          }
          td::uint64 amount = pending.amount + pending.fee;
          if (tail_started || amount < pending.amount || prefix_amount + amount < prefix_amount ||
              prefix_amount + amount > available_balance.value()) {
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

  const WalletMessageProcessor *wallet =
      acc.code.not_null() ? WalletMessageProcessor::get(acc.code->get_hash().bits()) : nullptr;
  if (wallet != nullptr) {
    msg_seqno = co_await check_message_to_wallet(message, wallet, std::move(acc), utime, lt, std::move(config),
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
