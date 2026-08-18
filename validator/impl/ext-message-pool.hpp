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
#pragma once

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include "interfaces/validator-manager.h"
#include "block/transaction.h"
#include "td/actor/coro_utils.h"
#include "td/utils/PersistentTreap.h"

#include "external-message.hpp"

namespace ton::validator {

class ExtMessagePool : public td::actor::Actor {
 public:
  ExtMessagePool(td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager)
      : opts_(opts), manager_(manager) {
  }

  struct CheckResult {
    td::Ref<ExtMessage> message;
    td::actor::StartedTask<> wait_allow_broadcast;
    bool should_broadcast{true};
    td::optional<td::uint32> msg_seqno;
    td::optional<block::NativeTransfer> native_transfer;
  };
  struct BatchCheckResult {
    ExternalMessageAdmissionResults statuses;
    std::vector<CheckResult> checked_messages;
  };
  td::actor::Task<CheckResult> check_add_external_message(td::BufferSlice data, int priority, bool add_to_mempool);
  td::actor::Task<CheckResult> check_add_external_message_until(td::BufferSlice data, int priority,
                                                                bool add_to_mempool,
                                                                td::Timestamp deadline);
  td::actor::Task<BatchCheckResult> check_add_external_messages_until(std::vector<td::BufferSlice> batch,
                                                                      int priority, bool add_to_mempool,
                                                                      td::Timestamp deadline);
  void install_collator_queue(ShardIdFull shard, std::unique_ptr<ExtMsgCallback> callback);
  void cleanup_external_messages(ShardIdFull shard);
  void complete_external_messages(std::vector<ExtMessage::Hash> to_delay, std::vector<ExtMessage::Hash> to_delete);
  void finalize_native_external_messages(std::vector<FinalizedNativeExternalMessage> messages);
  void erase_external_messages(std::vector<ExtMessage::Hash> to_delete);

  void update_last_masterchain_state(td::Ref<MasterchainState> state) {
    last_masterchain_state_ = std::move(state);
  }
  void update_options(td::Ref<ValidatorManagerOptions> opts) {
    opts_ = std::move(opts);
  }
  std::vector<std::pair<std::string, std::string>> prepare_stats();

  void start_up() override;
  void alarm() override;

 private:
  class NativeSignatureVerifier final : public td::actor::Actor {
   public:
    void verify(block::NativeTransfer transfer, Bits256 chain_domain, td::Promise<td::Unit> promise) {
      auto status = transfer.verify_signature(chain_domain);
      if (status.is_error()) {
        promise.set_error(std::move(status));
      } else {
        promise.set_value(td::Unit{});
      }
    }
  };

  struct MessageId {
    AccountIdPrefixFull dst;
    ExtMessage::Hash hash;

    bool operator<(const MessageId &msg) const {
      if (dst < msg.dst) {
        return true;
      }
      if (msg.dst < dst) {
        return false;
      }
      return hash < msg.hash;
    }
    bool operator==(const MessageId &msg) const {
      return !(*this < msg) && !(msg < *this);
    }
  };
  struct NativeMessageId {
    td::uint64 nonce;
    AccountIdPrefixFull dst;
    ExtMessage::Hash hash;

    bool operator<(const NativeMessageId &msg) const {
      if (nonce != msg.nonce) {
        return nonce < msg.nonce;
      }
      if (dst < msg.dst) {
        return true;
      }
      if (msg.dst < dst) {
        return false;
      }
      return hash < msg.hash;
    }
  };
  struct MempoolMsg {
    td::Ref<ExtMessage> message;
    ExtMessage::Hash hash_norm;
    td::uint32 generation = 0;
    bool active = true;
    td::Timestamp reactivate_at;
    td::Timestamp delete_at;
    td::optional<td::uint32> msg_seqno;
    td::optional<td::uint64> native_nonce;

    auto address() const {
      return std::make_pair(message->wc(), message->addr());
    }
    bool is_active() {
      if (!active) {
        if (reactivate_at.is_in_past()) {
          active = true;
          generation++;
        }
      }
      return active;
    }
    bool can_postpone() const {
      return generation <= 2;
    }
    void postpone() {
      if (!active) {
        return;
      }
      active = false;
      reactivate_at = td::Timestamp::in(generation * 5.0);
    }
    void postpone_native() {
      if (!active) {
        return;
      }
      active = false;
      // Native transfers are nonce-ordered.  Dropping a temporarily unprocessable
      // transfer can strand every later nonce from the same source, so retain it
      // until it is applied or expires and use a short capped retry backoff.
      auto exponent = std::min(generation, td::uint32{5});
      auto delay = std::min(1.0, 0.05 * static_cast<double>(1u << exponent));
      reactivate_at = td::Timestamp::in(delay);
    }
    bool expired() const {
      return delete_at.is_in_past();
    }
    void set_retention(double seconds) {
      delete_at = td::Timestamp::in(std::max(0.001, seconds));
    }
    explicit MempoolMsg(td::Ref<ExtMessage> msg) : message(std::move(msg)), hash_norm(message->hash_norm()) {
      set_retention(GENERIC_MEMPOOL_TTL_SECONDS);
    }

    static constexpr double GENERIC_MEMPOOL_TTL_SECONDS = 600.0;
  };

  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::Ref<MasterchainState> last_masterchain_state_;
  std::vector<td::actor::ActorOwn<NativeSignatureVerifier>> native_signature_verifiers_;
  std::size_t native_signature_verifier_cursor_{0};

  struct ExtMessages {
    td::PersistentTreap<MessageId, std::shared_ptr<MempoolMsg>> ext_messages_;
    td::PersistentTreap<MessageId, std::shared_ptr<MempoolMsg>> generic_messages_;
    td::PersistentTreap<NativeMessageId, std::shared_ptr<MempoolMsg>> native_messages_;
    std::map<std::pair<WorkchainId, StdSmcAddress>, std::map<ExtMessage::Hash, MessageId>> ext_addr_messages_;
  };
  struct NormalizedMessageId {
    int priority;
    MessageId id;

    bool operator<(const NormalizedMessageId &msg) const {
      if (priority != msg.priority) {
        return priority < msg.priority;
      }
      return id < msg.id;
    }
  };
  std::map<int, ExtMessages> ext_msgs_;                                        // priority -> messages
  std::map<ExtMessage::Hash, std::pair<int, MessageId>> ext_messages_hashes_;  // raw hash -> priority
  std::map<ExtMessage::Hash, std::set<NormalizedMessageId>> ext_messages_hashes_norm_;

  struct CheckedExtMsgCounter {
    std::map<std::pair<WorkchainId, StdSmcAddress>, size_t> counter_cur_, counter_prev_;
    td::Timestamp cleanup_at_ = td::Timestamp::now();

    size_t get_msg_count(WorkchainId wc, StdSmcAddress addr);
    size_t inc_msg_count(WorkchainId wc, StdSmcAddress addr);
    void before_query();
  } checked_ext_msg_counter_;
  td::uint64 total_check_ext_messages_ok_{0}, total_check_ext_messages_error_{0};
  td::uint64 native_batch_count_{0}, native_batch_messages_{0}, native_batch_unique_messages_{0};
  td::uint64 native_batch_account_lookups_{0}, native_batch_shard_fetches_{0};
  td::uint64 native_batch_accepted_{0}, native_batch_rejected_{0};
  td::Timestamp native_batch_log_at_ = td::Timestamp::now();
  td::uint64 applied_ext_msgs_delete_requests_{0}, applied_ext_msgs_deleted_{0};
  std::size_t native_collator_queue_limit_{32768};
  td::uint32 native_mempool_max_ttl_{3600};

  td::Timestamp cleanup_mempool_at_ = td::Timestamp::now();

  td::Status add_message_to_mempool(td::Ref<ExtMessage> message, int priority,
                                    td::optional<td::uint32> msg_seqno,
                                    const block::NativeTransfer *native_transfer = nullptr);
  td::Status commit_checked_message(td::Ref<ExtMessage> message, td::optional<td::uint32> msg_seqno,
                                    const block::NativeTransfer *native_transfer = nullptr);
  void rollback_checked_message(td::Ref<ExtMessage> message, td::optional<td::uint32> msg_seqno,
                                const block::NativeTransfer *native_transfer = nullptr);
  bool erase_message(int priority, const MessageId &id);

  struct WalletMessageInfo {
    td::uint32 valid_until;
    td::Promise<td::Unit> allow_broadcast_promise;
    bool committed{false};
  };
  struct WalletInfo {
    std::map<td::uint32, WalletMessageInfo> messages;
    td::uint32 observed_seqno{0};
    UnixTime observed_utime{0};
    ~WalletInfo() {
      for (auto &[_, message] : messages) {
        if (message.allow_broadcast_promise) {
          message.allow_broadcast_promise.set_error(td::Status::Error("wallet is no longer valid"));
        }
      }
    }
    void process_messages(td::uint32 wallet_seqno, UnixTime utime);
    bool commit_message(td::uint32 msg_seqno);
  };
  std::map<std::pair<WorkchainId, StdSmcAddress>, WalletInfo> wallets_;

  struct NativeMessageInfo {
    ExtMessage::Hash hash;
    td::uint64 amount;
    td::uint64 fee;
    td::uint32 valid_until;
    td::uint64 account_revision{0};
    td::Promise<td::Unit> allow_broadcast_promise;
    std::vector<td::Promise<td::Unit>> insertion_waiters;
    bool committed{false};

    void insertion_succeeded() {
      for (auto &waiter : insertion_waiters) {
        waiter.set_value(td::Unit{});
      }
      insertion_waiters.clear();
    }
    void insertion_failed(td::Slice reason) {
      for (auto &waiter : insertion_waiters) {
        waiter.set_error(td::Status::Error(reason));
      }
      insertion_waiters.clear();
    }
  };
  struct NativeInfo {
    std::map<td::uint64, NativeMessageInfo> messages;
    td::uint64 observed_nonce{0};
    UnixTime observed_utime{0};
    ~NativeInfo() {
      for (auto &[_, message] : messages) {
        if (message.allow_broadcast_promise) {
          message.allow_broadcast_promise.set_error(td::Status::Error("native account is no longer valid"));
        }
        message.insertion_failed("native account is no longer valid");
      }
    }
    std::vector<ExtMessage::Hash> process_messages(td::uint64 native_nonce, UnixTime utime);
    bool commit_message(td::uint64 native_nonce);
    td::uint64 reserved_amount_before(td::uint64 native_nonce, td::uint64 before_nonce) const;
  };
  using NativeAddress = std::pair<WorkchainId, StdSmcAddress>;
  struct NativeNonceWatermark {
    // Account nonce is the first nonce not consumed by the observed canonical
    // state. A separately recorded finalized nonce closes the window in which
    // an asynchronous verifier can resume with an older account fetch.
    td::uint64 observed_next_nonce{0};
    td::optional<td::uint64> highest_finalized_nonce;
    td::optional<td::uint64> observed_balance;
    LogicalTime observed_lt{0};
    td::uint64 revision{0};

    bool observe_account_state(td::uint64 nonce, td::uint64 balance, LogicalTime lt) {
      // A finalized debit is known before ValidatorManager necessarily exposes
      // the corresponding account state. Never bless balance from that older
      // snapshot; the caller retries after state catch-up.
      if (highest_finalized_nonce && nonce <= highest_finalized_nonce.value()) {
        return false;
      }
      if (observed_balance && lt < observed_lt) {
        return false;
      }
      bool changed = nonce > observed_next_nonce || !observed_balance ||
                     (lt >= observed_lt && balance != observed_balance.value());
      observed_next_nonce = std::max(observed_next_nonce, nonce);
      if (lt >= observed_lt) {
        observed_lt = lt;
        observed_balance = balance;
      }
      if (changed) {
        ++revision;
      }
      return true;
    }
    void observe_finalized_nonce(td::uint64 nonce) {
      if (!highest_finalized_nonce || nonce > highest_finalized_nonce.value()) {
        highest_finalized_nonce = nonce;
        // Finalization consumed source balance, but this callback does not
        // carry the new balance. Invalidate it and force a canonical refetch.
        observed_balance = {};
        ++revision;
      }
    }
    bool is_consumed(td::uint64 nonce) const {
      return nonce < observed_next_nonce ||
             (highest_finalized_nonce && nonce <= highest_finalized_nonce.value());
    }
    td::optional<td::uint64> first_unconsumed_nonce() const {
      if (!highest_finalized_nonce) {
        return observed_next_nonce;
      }
      if (highest_finalized_nonce.value() == std::numeric_limits<td::uint64>::max()) {
        return {};
      }
      return std::max(observed_next_nonce, highest_finalized_nonce.value() + 1);
    }
  };
  std::map<NativeAddress, NativeInfo> native_accounts_;
  // Do not discard a watermark when an account has no pending messages: an
  // older account-state fetch can still be suspended in a signature worker.
  std::map<NativeAddress, NativeNonceWatermark> native_nonce_watermarks_;

  td::actor::Task<CheckResult> check_message(td::Ref<ExtMessage> message,
                                             td::Timestamp deadline = td::Timestamp::never());
  td::actor::Task<CheckResult> check_add_parsed_external_message_until(td::Ref<ExtMessage> message,
                                                                       int priority, bool add_to_mempool,
                                                                       td::Timestamp deadline);
  td::Result<td::optional<CheckResult>> check_existing_external_message(td::Ref<ExtMessage> message,
                                                                        int priority, bool add_to_mempool);
  td::Result<CheckResult> finalize_checked_message(CheckResult result, int priority, bool add_to_mempool,
                                                   td::Timestamp deadline);
  td::actor::Task<CheckResult> reserve_verified_native_message(td::Ref<ExtMessage> message,
                                                               block::NativeTransfer transfer,
                                                               td::uint64 available_balance,
                                                               td::uint64 account_revision, UnixTime utime,
                                                               td::Timestamp deadline);
  void log_native_batch_stats();
  td::Result<td::uint32> check_message_to_wallet(td::Ref<ExtMessage> message, const WalletMessageProcessor *wallet,
                                                 block::Account acc, UnixTime utime, LogicalTime lt,
                                                 std::unique_ptr<block::ConfigInfo> config,
                                                 td::Promise<td::Unit> allow_broadcast_promise);

  std::vector<std::unique_ptr<ExtMsgCallback>> callbacks_;

  static constexpr double MAX_EXT_MSG_PER_ADDR_TIME_WINDOW = 10.0;
  static constexpr size_t MAX_EXT_MSG_PER_ADDR = 4096;
  static constexpr size_t PER_ADDRESS_LIMIT = 8192;
  static constexpr size_t SOFT_MEMPOOL_LIMIT = 262144;
  static constexpr size_t MAX_NATIVE_COLLATOR_QUEUE_LIMIT = 262144;
  static constexpr td::uint32 MAX_NATIVE_MEMPOOL_TTL = 86400;
  static constexpr td::uint32 MAX_WALLET_SEQNO_DIFF = 16;
  static constexpr td::uint64 MAX_NATIVE_NONCE_DIFF = 4096;
};

}  // namespace ton::validator
