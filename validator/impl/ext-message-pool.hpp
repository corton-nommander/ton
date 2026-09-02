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
#include <deque>
#include <limits>
#include <map>
#include <memory>
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

  // The pool schedules one physical native external at a time, but a v5
  // NativeTransferRun owns an indivisible contiguous source-nonce interval.
  // Keep the admission facts independent from the wire type so all pool
  // lifecycle paths (insert, commit, rollback, expiry and reconciliation)
  // use the same first-nonce/count pair for NTFX and NTRN.
  struct NativeAdmission {
    td::uint64 first_nonce{0};
    td::uint32 logical_count{0};
    td::uint64 amount{0};
    td::uint64 fee{0};
    UnixTime valid_until{0};
    // `logical_count == 1` is not sufficient to identify the wire format:
    // a source-signed run may legitimately contain one output. Keep the mode
    // explicit so a capability transition can evict only incompatible work.
    bool is_run{false};

    bool has_valid_interval() const {
      return logical_count != 0 &&
             first_nonce <= std::numeric_limits<td::uint64>::max() - (logical_count - 1);
    }
    td::optional<td::uint64> last_nonce() const {
      if (!has_valid_interval()) {
        return {};
      }
      return first_nonce + logical_count - 1;
    }
    td::optional<td::uint64> required_amount() const {
      if (amount > std::numeric_limits<td::uint64>::max() - fee) {
        return {};
      }
      return amount + fee;
    }
  };

  struct CheckResult {
    td::Ref<ExtMessage> message;
    td::actor::StartedTask<> wait_allow_broadcast;
    bool should_broadcast{true};
    td::optional<td::uint32> msg_seqno;
    td::optional<NativeAdmission> native_admission;
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
  // Local consensus acceptance is only a hint that these sources may advance.
  // It never changes admission watermarks or removes pool entries.
  void track_locally_accepted_native_messages(std::vector<TrackedNativeExternalMessage> messages);
  // The shard client calls this with a masterchain state only after every
  // referenced shard top has been applied. Reconciliation reads those exact
  // shard account states and is the sole authority for native prefix purges.
  void reconcile_native_external_messages(td::Ref<MasterchainState> state);
  void erase_external_messages(std::vector<ExtMessage::Hash> to_delete);

  void update_last_masterchain_state(td::Ref<MasterchainState> state);
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
    void verify_run(block::NativeTransferRun run, Bits256 chain_domain, td::Promise<td::Unit> promise) {
      auto status = run.verify_signature(chain_domain);
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
    // This is distinct from `active`: it tracks whether the object is still
    // present in the pool indices. Native reservations retain a direct shared
    // link, so erasure must make a stale link observable before the treap
    // releases its ownership.
    bool in_mempool{false};
    td::Timestamp reactivate_at;
    td::Timestamp delete_at;
    td::optional<td::uint32> msg_seqno;
    // A native external is one physical BOC but can eventually represent an
    // atomic contiguous nonce interval.  Existing NTFX transfers always keep
    // the scalar shape (native_nonce_count == 1).  Keep the interval next to
    // the pool object so a scheduler never needs to infer logical work from
    // the external payload while it is on its hot path.
    td::optional<td::uint64> native_nonce;
    td::uint32 native_nonce_count{0};
    bool native_is_run{false};

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
    bool postpone_native() {
      if (!active) {
        return false;
      }
      active = false;
      // Native transfers are nonce-ordered.  Dropping a temporarily unprocessable
      // transfer can strand every later nonce from the same source, so retain it
      // until it is applied or expires and use a short capped retry backoff.
      auto exponent = std::min(generation, td::uint32{5});
      auto delay = std::min(1.0, 0.05 * static_cast<double>(1u << exponent));
      reactivate_at = td::Timestamp::in(delay);
      return true;
    }
    bool expired() const {
      return delete_at.is_in_past();
    }
    bool has_native_interval() const {
      return native_nonce && native_nonce_count != 0;
    }
    td::optional<td::uint64> native_last_nonce() const {
      if (!has_native_interval()) {
        return {};
      }
      const auto first = native_nonce.value();
      const auto count = native_nonce_count;
      if (first > std::numeric_limits<td::uint64>::max() - (count - 1)) {
        return {};
      }
      return first + count - 1;
    }
    bool covers_native_nonce(td::uint64 nonce) const {
      auto last = native_last_nonce();
      return last && native_nonce.value() <= nonce && nonce <= last.value();
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
  td::uint64 native_batch_account_lookups_{0};
  // Counts ExtMessagePool cache misses handed to ValidatorManager. The
  // manager may satisfy one from its own exact-ID cache or join an existing
  // exact-ID waiter, so this is deliberately not a physical DB/network count.
  td::uint64 native_batch_shard_manager_waits_{0};
  td::uint64 native_batch_shard_state_requests_{0}, native_batch_shard_cache_hits_{0};
  td::uint64 native_batch_shard_cache_fills_{0}, native_batch_shard_cache_fill_races_{0};
  td::uint64 native_batch_shard_cache_fill_conflicts_{0}, native_batch_shard_cache_generation_resets_{0};
  td::uint64 native_batch_shard_cache_stale_generation_fill_skips_{0};
  td::uint64 native_batch_shard_cache_wrong_id_{0}, native_batch_shard_cache_invalid_header_{0};
  td::uint64 native_batch_shard_miss_errors_{0}, native_batch_shard_cache_peak_entries_{0};
  td::uint64 native_batch_shard_manager_wait_errors_{0};
  td::uint64 native_batch_shard_manager_wait_timeouts_{0}, native_batch_shard_manager_wait_notready_{0};
  td::uint64 native_batch_shard_manager_wait_other_errors_{0};
  td::uint64 native_batch_shard_manager_wait_late_results_{0};
  td::uint64 native_batch_accepted_{0}, native_batch_rejected_{0};
  td::uint64 native_batch_mc_state_pins_{0}, native_batch_ignored_mc_state_updates_{0};
  td::uint64 native_batch_last_pinned_mc_seqno_{0}, native_batch_last_pinned_shard_seqno_{0};
  td::uint64 native_batch_max_mc_shard_utime_lag_s_{0};
  td::uint64 native_batch_watermark_lag_rejections_{0}, native_batch_max_watermark_nonce_lag_{0};
  td::uint64 native_reconciliation_tracked_candidates_{0}, native_reconciliation_tracked_messages_{0};
  td::uint64 native_reconciliation_runs_{0}, native_reconciliation_state_fetches_{0};
  td::uint64 native_reconciliation_account_lookups_{0}, native_reconciliation_sources_advanced_{0};
  td::uint64 native_reconciliation_messages_purged_{0}, native_reconciliation_failures_{0};
  td::uint64 native_reconciliation_unchanged_top_skips_{0};
  td::uint64 native_reconciliation_unchanged_source_skips_{0};
  td::uint64 native_reconciliation_unchanged_state_skips_{0};
  td::uint64 native_reconciliation_rebased_reservations_{0};
  td::uint64 native_reconciliation_unaffordable_tail_pruned_{0};
  td::uint64 native_reconciliation_stale_uncommitted_tail_pruned_{0};
  td::uint64 native_exact_retry_preserved_stale_revision_{0};
  td::uint64 native_expiry_suffix_events_{0}, native_expiry_suffix_pruned_{0};
  td::uint64 native_reconciliation_last_mc_seqno_{0}, native_reconciliation_last_shard_seqno_{0};
  std::map<ShardIdFull, BlockIdExt> native_reconciliation_successful_shard_tops_;
  using NativeShardTopFingerprint = std::map<ShardIdFull, BlockIdExt>;
  td::optional<NativeShardTopFingerprint> native_reconciliation_successful_state_fingerprint_;
  td::Timestamp native_batch_log_at_ = td::Timestamp::now();
  td::uint64 applied_ext_msgs_delete_requests_{0}, applied_ext_msgs_deleted_{0};
  std::size_t native_collator_queue_limit_{32768};
  td::uint32 native_mempool_max_ttl_{3600};

  // Admission only consumes this immutable projection of an exact shard state.
  // The full BlockIdExt is the lookup key, while the state root detects an
  // impossible conflicting fill for the same block identity.
  struct NativeAdmissionShardView {
    BlockIdExt block_id;
    RootHash state_root_hash;
    UnixTime gen_utime{0};
    LogicalTime gen_lt{0};
    td::Ref<vm::Cell> accounts;
  };
  using NativeAdmissionShardViewPtr = std::shared_ptr<const NativeAdmissionShardView>;
  struct NativeAdmissionShardCache {
    td::optional<BlockIdExt> masterchain_block_id;
    std::map<BlockIdExt, NativeAdmissionShardViewPtr> shard_views;
  } native_admission_shard_cache_;

  struct NativeAdmissionSnapshot {
    td::Ref<MasterchainState> state;
    BlockIdExt block_id;
    Bits256 chain_domain;
    bool runs_enabled{false};
  };

  td::Timestamp cleanup_mempool_at_ = td::Timestamp::now();

  td::Status add_message_to_mempool(td::Ref<ExtMessage> message, int priority,
                                    td::optional<td::uint32> msg_seqno,
                                    const NativeAdmission *native_admission = nullptr);
  td::Status commit_checked_message(td::Ref<ExtMessage> message, td::optional<td::uint32> msg_seqno,
                                    const NativeAdmission *native_admission = nullptr);
  void rollback_checked_message(td::Ref<ExtMessage> message, td::optional<td::uint32> msg_seqno,
                                const NativeAdmission *native_admission = nullptr);
  bool erase_message(int priority, const MessageId &id, bool prune_expired_suffix = true);

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

  using NativeAddress = std::pair<WorkchainId, StdSmcAddress>;

  // One NativeWork owns the whole source-contiguous interval represented by a
  // native external.  The map below is keyed by first_nonce, never by every
  // logical output.  Scalar NTFX uses logical_count=1, which keeps the
  // existing pool layout and scheduling behaviour byte-for-byte equivalent.
  // A future NTRN admission path can populate a work with logical_count > 1
  // without allowing a checkpoint, expiry, or rollback to split it.
  struct NativeWork {
    struct MempoolLink {
      std::shared_ptr<MempoolMsg> message;
      int priority;
      MessageId id;
    };

    ExtMessage::Hash hash;
    NativeAddress source;
    td::uint32 logical_count{1};
    // Aggregate debit across every logical output in the interval.  This is
    // deliberately not a per-output value: balance admission/rebasing must
    // retain or reject the signed work as a whole.
    td::uint64 amount;
    td::uint64 fee;
    td::uint32 valid_until;
    bool is_run{false};
    td::uint64 account_revision{0};
    td::Promise<td::Unit> allow_broadcast_promise;
    std::vector<td::Promise<td::Unit>> insertion_waiters;
    bool committed{false};
    // A committed reservation is normally probed many times while a collator
    // callback is filled. Keep the pool object and its priority beside the
    // nonce reservation so that the hot path does not repeat raw-hash and
    // persistent-treap lookups. The link is only trusted after its liveness
    // and immutable identity are checked; otherwise probing takes the legacy
    // lookup path and repairs it.
    td::optional<MempoolLink> mempool_link;

    bool has_valid_interval(td::uint64 first_nonce) const {
      return logical_count != 0 && first_nonce <= std::numeric_limits<td::uint64>::max() - (logical_count - 1);
    }
    td::optional<td::uint64> last_nonce(td::uint64 first_nonce) const {
      if (!has_valid_interval(first_nonce)) {
        return {};
      }
      return first_nonce + logical_count - 1;
    }
    bool covers_nonce(td::uint64 first_nonce, td::uint64 nonce) const {
      auto last = last_nonce(first_nonce);
      return last && first_nonce <= nonce && nonce <= last.value();
    }
    td::optional<td::uint64> required_amount() const {
      if (amount > std::numeric_limits<td::uint64>::max() - fee) {
        return {};
      }
      return amount + fee;
    }

    void set_mempool_link(std::shared_ptr<MempoolMsg> message, int priority, MessageId id) {
      mempool_link = MempoolLink{std::move(message), priority, std::move(id)};
    }
    void clear_mempool_link() {
      mempool_link = {};
    }

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
  struct NativeMessageProcessResult {
    std::vector<ExtMessage::Hash> obsolete_hashes;
    td::uint64 expired_suffix_pruned{0};
  };
  struct NativeInfo {
    std::map<td::uint64, NativeWork> messages;
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
    using WorkIterator = std::map<td::uint64, NativeWork>::iterator;
    using ConstWorkIterator = std::map<td::uint64, NativeWork>::const_iterator;

    WorkIterator find_work_covering(td::uint64 nonce);
    ConstWorkIterator find_work_covering(td::uint64 nonce) const;
    bool interval_overlaps(td::uint64 first_nonce, td::uint32 logical_count) const;
    NativeMessageProcessResult process_messages(td::uint64 native_nonce, UnixTime utime);
    bool commit_message(td::uint64 native_nonce, NativeMessageProcessResult &processed);
    td::uint64 reserved_amount_before(td::uint64 native_nonce, td::uint64 before_nonce) const;
  };
  struct NativeNonceWatermark {
    // Account nonce is the first nonce not consumed by observed canonical
    // state. Only account states referenced by an applied masterchain state
    // may advance it; local consensus acceptance is never authoritative.
    td::uint64 observed_next_nonce{0};
    td::optional<td::uint64> observed_balance;
    LogicalTime observed_lt{0};
    td::uint64 revision{0};

    bool observe_account_state(td::uint64 nonce, td::uint64 balance, LogicalTime lt) {
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
    bool is_consumed(td::uint64 nonce) const {
      return nonce < observed_next_nonce;
    }
    td::optional<td::uint64> first_unconsumed_nonce() const {
      return observed_next_nonce;
    }
  };
  std::map<NativeAddress, NativeInfo> native_accounts_;
  // The applied configuration selects exactly one native wire format. A
  // one-output NTRN is still a run, so this cannot be inferred from interval
  // length when clearing an incompatible mode after a config transition.
  bool native_transfer_runs_mode_initialized_{false};
  bool native_transfer_runs_mode_enabled_{false};
  // Do not discard a watermark when an account has no pending messages: an
  // older account-state fetch can still be suspended in a signature worker.
  std::map<NativeAddress, NativeNonceWatermark> native_nonce_watermarks_;
  // Sources that need comparison with the next applied canonical shard state.
  // Local accepts add an early hint, while every applied masterchain update
  // also adds all sources that still have native reservations.  The latter is
  // the correctness fallback for blocks learned through sync or another
  // validator group rather than accepted by this process.
  std::map<NativeAddress, td::uint64> locally_accepted_native_nonces_;
  td::Ref<MasterchainState> applied_reconciliation_state_;
  td::uint64 native_reconciliation_generation_{0};
  bool native_reconciliation_active_{false};

  struct NativeQueueCounters {
    td::uint64 installs{0};
    td::uint64 masterchain_installs{0};
    td::uint64 scanned{0};
    td::uint64 direct_link_hits{0};
    td::uint64 direct_link_fallbacks{0};
    // selected is physical native BOCs handed to the transport; logical_selected
    // is the total source nonce/candidate capacity represented by them.
    td::uint64 selected{0};
    td::uint64 logical_selected{0};
    td::uint64 active{0};
    td::uint64 inactive{0};
    td::uint64 excluded{0};
    td::uint64 expired{0};
    td::uint64 already_delivered{0};
    td::uint64 ready_sources{0};
    td::uint64 head_gaps{0};
    td::uint64 head_missing_watermark{0};
    td::uint64 head_missing_nonce{0};
    td::uint64 head_uncommitted{0};
    td::uint64 head_missing_hash_index{0};
    td::uint64 head_missing_priority{0};
    td::uint64 head_missing_message{0};
    td::uint64 head_nonce_mismatch{0};
    td::uint64 speculative_exhausted{0};
    td::uint64 runs{0};
    td::uint64 run_messages{0};
    td::uint64 max_run_size{0};
    td::uint64 delayed{0};
    td::uint64 reactivated{0};
    td::uint64 reactivation_wakes{0};
    td::uint64 scheduler_builds{0};
    td::uint64 source_scans{0};
    td::uint64 source_refreshes{0};
    td::uint64 source_probes{0};
    td::uint64 stale_ready_tokens{0};

    void add(const NativeQueueCounters &other) {
      installs += other.installs;
      masterchain_installs += other.masterchain_installs;
      scanned += other.scanned;
      direct_link_hits += other.direct_link_hits;
      direct_link_fallbacks += other.direct_link_fallbacks;
      selected += other.selected;
      logical_selected += other.logical_selected;
      active += other.active;
      inactive += other.inactive;
      excluded += other.excluded;
      expired += other.expired;
      already_delivered += other.already_delivered;
      ready_sources += other.ready_sources;
      head_gaps += other.head_gaps;
      head_missing_watermark += other.head_missing_watermark;
      head_missing_nonce += other.head_missing_nonce;
      head_uncommitted += other.head_uncommitted;
      head_missing_hash_index += other.head_missing_hash_index;
      head_missing_priority += other.head_missing_priority;
      head_missing_message += other.head_missing_message;
      head_nonce_mismatch += other.head_nonce_mismatch;
      speculative_exhausted += other.speculative_exhausted;
      runs += other.runs;
      run_messages += other.run_messages;
      max_run_size = std::max(max_run_size, other.max_run_size);
      delayed += other.delayed;
      reactivated += other.reactivated;
      reactivation_wakes += other.reactivation_wakes;
      scheduler_builds += other.scheduler_builds;
      source_scans += other.source_scans;
      source_refreshes += other.source_refreshes;
      source_probes += other.source_probes;
      stale_ready_tokens += other.stale_ready_tokens;
    }
  } native_queue_counters_;

  struct NativeQueueItem {
    td::Ref<ExtMessage> message;
    int priority{0};
    NativeAddress source;
    td::uint64 nonce{0};
    // Physical queue entries are one BOC per NativeQueueItem.  This is the
    // number of source nonce steps / candidate slots represented by that BOC.
    td::uint32 logical_count{1};
  };
  struct NativeQueueSelection {
    std::vector<NativeQueueItem> items;
    std::size_t logical_count{0};
    td::optional<NativeAddress> cursor;
    td::Timestamp earliest_reactivation;
    NativeQueueCounters counters;
  };
  struct CallbackNativeSource {
    td::uint64 next_nonce{0};
    td::uint64 generation{0};
    td::optional<NativeQueueItem> next;
    bool queued{false};
    bool ready_counted{false};
  };
  struct CallbackNativeReadyToken {
    NativeAddress source;
    td::uint64 generation{0};
  };
  struct CallbackNativeScheduler {
    bool initialized{false};
    std::map<NativeAddress, CallbackNativeSource> sources;
    std::map<int, std::deque<CallbackNativeReadyToken>> ready_by_priority;
  };
  struct InstalledCallback {
    explicit InstalledCallback(std::unique_ptr<ExtMsgCallback> value) : callback(std::move(value)) {
    }

    std::unique_ptr<ExtMsgCallback> callback;
    std::deque<ExtMsgQueueEntry> pending_native;
    std::deque<ExtMsgQueueEntry> pending_generic;
    CallbackNativeScheduler native_scheduler;
    // Live ingress coalesces source-local scheduler invalidations here while
    // the serialized pump is suspended on queue backpressure. The pump applies
    // them immediately before its next demand-driven refill.
    std::set<NativeAddress> native_dirty_sources;
    bool native_scheduler_rebuild{false};
    std::set<ExtMessage::Hash> delivered_native;
    td::uint64 delivered_native_logical{0};
    td::optional<NativeAddress> native_cursor;
    std::size_t generic_selected{0};
    td::uint64 completion_epoch{0};
    td::uint64 producer_epoch{0};
    bool pump_active{false};
    bool producer_epoch_open{false};
    bool native_snapshot_exhausted{false};
  };

  td::optional<NativeAddress> native_scheduler_cursor_;
  std::multimap<td::Timestamp, std::pair<int, MessageId>> native_reactivations_;
  std::shared_ptr<ExtMsgQueueTelemetry> native_transport_telemetry_{std::make_shared<ExtMsgQueueTelemetry>()};

  NativeQueueSelection select_native_messages(
      ShardIdFull shard, const std::vector<ExtMessage::Hash> &excluded_messages,
      const std::set<ExtMessage::Hash> &already_delivered, std::size_t limit,
      td::optional<NativeAddress> cursor, const std::set<NativeAddress> *source_filter = nullptr);
  NativeQueueSelection select_callback_native_messages(const std::shared_ptr<InstalledCallback> &callback,
                                                       std::size_t logical_limit, std::size_t physical_limit,
                                                       const std::set<NativeAddress> *source_filter = nullptr);
  void initialize_callback_native_scheduler(const std::shared_ptr<InstalledCallback> &callback,
                                            NativeQueueCounters &counters);
  void refresh_callback_native_source(const std::shared_ptr<InstalledCallback> &callback,
                                      const NativeAddress &source, NativeQueueCounters &counters,
                                      bool count_refresh);
  void probe_callback_native_source(const std::shared_ptr<InstalledCallback> &callback,
                                    const NativeAddress &source, CallbackNativeSource &state,
                                    NativeQueueCounters &counters, bool enqueue_ready);
  void enqueue_callback_native_source(CallbackNativeScheduler &scheduler, const NativeAddress &source,
                                      CallbackNativeSource &state);
  std::size_t fill_callback_native(const std::shared_ptr<InstalledCallback> &callback,
                                   bool count_install = false,
                                   const std::set<NativeAddress> *source_filter = nullptr,
                                   std::size_t max_items = NATIVE_DELIVERY_CHUNK);
  std::size_t prefill_callback_native(const std::shared_ptr<InstalledCallback> &callback, bool count_install);
  std::size_t native_transport_selected_limit(const InstalledCallback &callback) const;
  bool native_transport_has_refill_credit(const InstalledCallback &callback) const;
  std::size_t wake_native_callbacks(const std::set<NativeAddress> *source_filter = nullptr,
                                    bool preserve_valid_ready_head = false);
  std::size_t reactivate_due_native_messages(td::Timestamp now);
  void enqueue_callback_item(const std::shared_ptr<InstalledCallback> &callback,
                             std::pair<td::Ref<ExtMessage>, int> item, bool native,
                             td::uint32 native_logical_count = 1);
  void begin_callback_epoch(const std::shared_ptr<InstalledCallback> &callback);
  void start_callback_pump(const std::shared_ptr<InstalledCallback> &callback);
  void cancel_callback_delivery(const std::shared_ptr<InstalledCallback> &callback);
  td::actor::Task<> pump_callback(std::shared_ptr<InstalledCallback> callback);

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
                                                               NativeAdmission native_admission,
                                                               td::uint64 available_balance,
                                                               td::uint64 account_revision, UnixTime utime,
                                                               td::Timestamp deadline);
  static td::Result<NativeAdmission> make_native_admission(const block::NativeTransfer &transfer);
  static td::Result<NativeAdmission> make_native_admission(const block::NativeTransferRun &run);
  static td::Result<td::optional<NativeAdmission>> parse_native_admission(td::Ref<vm::Cell> root);
  static bool native_transfer_runs_enabled(int global_version, bool has_capabilities, long long capabilities);
  static bool native_transfer_runs_enabled(const block::ConfigInfo &config);
  static td::Status validate_native_transfer_run_locality(const block::NativeTransferRun &run,
                                                           const MasterchainState &state);
  td::Result<td::Ref<MasterchainState>> pin_native_admission_masterchain_state() const;
  td::Result<NativeAdmissionSnapshot> pin_native_admission_snapshot();
  td::Status validate_native_admission_mode(const NativeAdmissionSnapshot &snapshot, bool is_run) const;
  void update_native_transfer_runs_mode(bool enabled);
  void purge_incompatible_native_messages(bool runs_enabled);
  void refresh_native_transfer_runs_mode_from_applied_state();
  void reset_native_admission_cache_generation(const BlockIdExt &masterchain_block_id);
  NativeAdmissionShardViewPtr lookup_native_admission_shard_view(
      const BlockIdExt &masterchain_block_id, const BlockIdExt &shard_block_id);
  td::Result<NativeAdmissionShardViewPtr> make_native_admission_shard_view(
      const BlockIdExt &shard_block_id, td::Ref<ShardState> state);
  td::Result<NativeAdmissionShardViewPtr> store_native_admission_shard_view(
      const BlockIdExt &masterchain_block_id, NativeAdmissionShardViewPtr view);
  void record_native_admission_manager_wait_error(const td::Status &error);
  bool native_admission_manager_wait_finished_after_deadline(td::Timestamp deadline);
  void register_pending_native_reconciliation_targets();
  void prune_native_reconciliation_target_if_idle(const NativeAddress &address);
  void start_native_reconciliation();
  td::actor::Task<> run_native_reconciliation();
  td::actor::Task<> reconcile_native_snapshot(td::Ref<MasterchainState> state,
                                              std::vector<NativeAddress> sources);
  bool should_reconcile_native_shard_top(const BlockIdExt &shard_block_id, std::size_t source_count);
  void record_successful_native_shard_reconciliation(const BlockIdExt &shard_block_id);
  NativeShardTopFingerprint native_reconciliation_state_fingerprint(const td::Ref<MasterchainState> &state) const;
  bool should_skip_native_reconciliation_state(const NativeShardTopFingerprint &fingerprint);
  void record_successful_native_reconciliation_state(NativeShardTopFingerprint fingerprint);
  td::Result<bool> apply_canonical_native_account_state(const NativeAddress &address, td::uint64 native_nonce,
                                                        td::uint64 balance, UnixTime utime, LogicalTime lt);
  td::uint64 erase_processed_native_messages(NativeMessageProcessResult processed);
  td::uint64 prune_expired_native_suffix(const NativeAddress &address, td::uint64 from_nonce,
                                         td::Slice reason);
  void log_native_batch_stats();
  td::Result<td::uint32> check_message_to_wallet(td::Ref<ExtMessage> message, const WalletMessageProcessor *wallet,
                                                 block::Account acc, UnixTime utime, LogicalTime lt,
                                                 std::unique_ptr<block::ConfigInfo> config,
                                                 td::Promise<td::Unit> allow_broadcast_promise);

  std::vector<std::shared_ptr<InstalledCallback>> callbacks_;

  friend class ExtMessagePoolTestAccess;

  static constexpr double MAX_EXT_MSG_PER_ADDR_TIME_WINDOW = 10.0;
  static constexpr size_t MAX_EXT_MSG_PER_ADDR = 4096;
  static constexpr size_t PER_ADDRESS_LIMIT = 8192;
  static constexpr size_t SOFT_MEMPOOL_LIMIT = 262144;
  // One compact native block cannot encode more entries than this. Selecting
  // additional references only leaves a large callback backlog that the
  // candidate can never consume.
  static constexpr size_t MAX_NATIVE_COLLATOR_QUEUE_LIMIT = 65536;
  static constexpr size_t NATIVE_DELIVERY_CHUNK = 512;
  static constexpr size_t NATIVE_SOURCE_RUN_TARGET = 16;
  static constexpr size_t STANDARD_COLLATOR_QUEUE_LIMIT = 500;
  static constexpr td::uint32 MAX_NATIVE_MEMPOOL_TTL = 86400;
  static constexpr td::uint32 MAX_WALLET_SEQNO_DIFF = 16;
  static constexpr td::uint64 MAX_NATIVE_NONCE_DIFF = 4096;
};

}  // namespace ton::validator
