#include <array>
#include <tuple>

#include "td/utils/tests.h"
#include "td/actor/TestScheduler.h"
#include "td/actor/SharedFuture.h"
#include "validator/consensus/manager-facade.h"
#include "validator/impl/ext-message-pool.hpp"
#include "validator/impl/shard.hpp"
#include "validator/impl/applied-ext-message-cleanup.hpp"
#include "validator/manager.hpp"

namespace ton::validator {
namespace {

Bits256 make_bits(td::uint32 value, unsigned prefix = 0) {
  Bits256 result = Bits256::zero();
  result.as_array()[0] = static_cast<unsigned char>(prefix);
  result.as_array()[28] = static_cast<unsigned char>(value >> 24);
  result.as_array()[29] = static_cast<unsigned char>(value >> 16);
  result.as_array()[30] = static_cast<unsigned char>(value >> 8);
  result.as_array()[31] = static_cast<unsigned char>(value);
  return result;
}

class FakeExtMessage final : public ExtMessage {
 public:
  FakeExtMessage(StdSmcAddress source, Hash hash)
      : source_(source), hash_(hash), shard_(extract_addr_prefix(basechainId, source)) {
  }

  AccountIdPrefixFull shard() const override {
    return shard_;
  }
  td::BufferSlice serialize() const override {
    return {};
  }
  td::Ref<vm::Cell> root_cell() const override {
    return {};
  }
  Hash hash() const override {
    return hash_;
  }
  Hash hash_norm() const override {
    return hash_;
  }
  WorkchainId wc() const override {
    return basechainId;
  }
  StdSmcAddress addr() const override {
    return source_;
  }

 private:
  StdSmcAddress source_;
  Hash hash_;
  AccountIdPrefixFull shard_;
};

}  // namespace

class ExtMessagePoolTestAccess {
 public:
  using NativeAddress = ExtMessagePool::NativeAddress;

  struct Selection {
    std::vector<NativeAddress> sources;
    std::vector<td::uint64> nonces;
    std::vector<td::uint32> logical_counts;
    td::optional<NativeAddress> cursor;
    td::uint64 scanned{0};
    td::uint64 selected{0};
    td::uint64 logical_selected{0};
    td::uint64 inactive{0};
    td::uint64 excluded{0};
    td::uint64 ready_sources{0};
    td::uint64 head_gaps{0};
    td::uint64 head_missing_nonce{0};
    td::uint64 runs{0};
    td::uint64 max_run_size{0};
  };
  struct SchedulerStats {
    td::uint64 selected{0};
    td::uint64 direct_link_hits{0};
    td::uint64 direct_link_fallbacks{0};
    td::uint64 builds{0};
    td::uint64 source_scans{0};
    td::uint64 source_refreshes{0};
    td::uint64 source_probes{0};
    td::uint64 runs{0};
    td::uint64 inactive{0};
    td::uint64 excluded{0};
    td::uint64 head_gaps{0};
  };
  struct CallbackRound {
    std::vector<NativeAddress> sources;
    std::vector<td::uint64> nonces;
    std::vector<td::uint32> logical_counts;
    td::optional<NativeAddress> cursor;
    td::uint64 source_scans{0};
    td::uint64 scanned{0};
    td::uint64 excluded{0};
  };

  static bool malformed_config_is_never_cached() {
    auto pool = make_pool();
    pool.last_masterchain_state_ = masterchain_state(42);
    for (unsigned i = 0; i < 2; ++i) {
      if (pool.pin_native_admission_snapshot().is_ok()) { return false; }
    }
    return pool.native_config_cache_hits_ == 0 && pool.native_config_cache_misses_ == 2 &&
           pool.native_config_errors_ == 2;
  }

  static ExtMessagePool make_pool() {
    return ExtMessagePool({}, {});
  }

  static NativeAddress source(unsigned prefix) {
    return {basechainId, make_bits(0, prefix)};
  }

  static td::Ref<MasterchainState> masterchain_state(BlockSeqno seqno, td::uint32 revision = 0) {
    auto id = BlockIdExt{BlockId{masterchainId, shardIdAll, seqno}, make_bits(seqno, revision),
                         make_bits(seqno + 1, revision)};
    return td::make_ref<MasterchainStateQ>(id, td::Ref<vm::Cell>{});
  }

  static BlockIdExt shard_top(BlockSeqno seqno, td::uint32 revision = 0) {
    return BlockIdExt{BlockId{basechainId, shardIdAll, seqno}, make_bits(seqno, revision),
                      make_bits(seqno + 1, revision)};
  }

  static td::Ref<MasterchainState> pin_native_admission_state(ExtMessagePool &pool) {
    auto result = pool.pin_native_admission_masterchain_state();
    CHECK(result.is_ok());
    return result.move_as_ok();
  }

  static td::uint64 ignored_masterchain_updates(const ExtMessagePool &pool) {
    return pool.native_batch_ignored_mc_state_updates_;
  }

  struct AdmissionCacheStats {
    td::uint64 requests{0};
    td::uint64 hits{0};
    td::uint64 manager_waits{0};
    td::uint64 fills{0};
    td::uint64 fill_races{0};
    td::uint64 fill_conflicts{0};
    td::uint64 generation_resets{0};
    td::uint64 stale_generation_fill_skips{0};
    td::uint64 wrong_id{0};
    td::uint64 invalid_header{0};
    td::uint64 miss_errors{0};
    td::uint64 manager_wait_errors{0};
    td::uint64 manager_wait_timeouts{0};
    td::uint64 manager_wait_notready{0};
    td::uint64 manager_wait_other_errors{0};
    td::uint64 manager_wait_late_results{0};
    td::uint64 entries{0};
    td::uint64 peak_entries{0};
  };

  static ExtMessagePool::NativeAdmissionShardViewPtr admission_view(const BlockIdExt &block_id,
                                                                     td::uint32 state_revision,
                                                                     UnixTime gen_utime = 100,
                                                                     LogicalTime gen_lt = 200) {
    auto accounts = vm::CellBuilder{}.store_long(state_revision, 32).finalize_novm();
    return std::make_shared<const ExtMessagePool::NativeAdmissionShardView>(
        ExtMessagePool::NativeAdmissionShardView{.block_id = block_id,
                                                 .state_root_hash = make_bits(state_revision, 200),
                                                 .gen_utime = gen_utime,
                                                 .gen_lt = gen_lt,
                                                 .accounts = std::move(accounts)});
  }

  static ExtMessagePool::NativeAdmissionShardViewPtr lookup_admission_view(
      ExtMessagePool &pool, const BlockIdExt &masterchain_block_id, const BlockIdExt &shard_block_id) {
    return pool.lookup_native_admission_shard_view(masterchain_block_id, shard_block_id);
  }

  static void reset_admission_generation(ExtMessagePool &pool, const BlockIdExt &masterchain_block_id) {
    pool.reset_native_admission_cache_generation(masterchain_block_id);
  }

  static td::Result<ExtMessagePool::NativeAdmissionShardViewPtr> store_admission_view(
      ExtMessagePool &pool, const BlockIdExt &masterchain_block_id,
      ExtMessagePool::NativeAdmissionShardViewPtr view) {
    return pool.store_native_admission_shard_view(masterchain_block_id, std::move(view));
  }

  static td::Result<ExtMessagePool::NativeAdmissionShardViewPtr> make_admission_view(
      ExtMessagePool &pool, const BlockIdExt &shard_block_id, td::Ref<ShardState> state) {
    return pool.make_native_admission_shard_view(shard_block_id, std::move(state));
  }

  static AdmissionCacheStats admission_cache_stats(const ExtMessagePool &pool) {
    return AdmissionCacheStats{
        .requests = pool.native_batch_shard_state_requests_,
        .hits = pool.native_batch_shard_cache_hits_,
        .manager_waits = pool.native_batch_shard_manager_waits_,
        .fills = pool.native_batch_shard_cache_fills_,
        .fill_races = pool.native_batch_shard_cache_fill_races_,
        .fill_conflicts = pool.native_batch_shard_cache_fill_conflicts_,
        .generation_resets = pool.native_batch_shard_cache_generation_resets_,
        .stale_generation_fill_skips = pool.native_batch_shard_cache_stale_generation_fill_skips_,
        .wrong_id = pool.native_batch_shard_cache_wrong_id_,
        .invalid_header = pool.native_batch_shard_cache_invalid_header_,
        .miss_errors = pool.native_batch_shard_miss_errors_,
        .manager_wait_errors = pool.native_batch_shard_manager_wait_errors_,
        .manager_wait_timeouts = pool.native_batch_shard_manager_wait_timeouts_,
        .manager_wait_notready = pool.native_batch_shard_manager_wait_notready_,
        .manager_wait_other_errors = pool.native_batch_shard_manager_wait_other_errors_,
        .manager_wait_late_results = pool.native_batch_shard_manager_wait_late_results_,
        .entries = pool.native_admission_shard_cache_.shard_views.size(),
        .peak_entries = pool.native_batch_shard_cache_peak_entries_,
    };
  }

  using ShardRequest = ExtMessagePool::NativeAdmissionShardRequest;
  using ShardViewPtr = ExtMessagePool::NativeAdmissionShardViewPtr;

  static void enable_shared_admission(ExtMessagePool &pool, bool enabled) {
    pool.native_admission_shard_sharing_enabled_ = enabled;
  }

  static td::actor::Task<ShardViewPtr> wait_admission_view(ExtMessagePool &pool, BlockIdExt mc,
                                                        BlockIdExt shard, td::Timestamp deadline) {
    co_return co_await pool.wait_native_admission_shard_view(mc, shard, deadline);
  }

  static td::optional<ShardRequest> queue_admission_view(ExtMessagePool &pool, const BlockIdExt &mc,
                                                       const BlockIdExt &shard, td::Timestamp deadline) {
    return pool.queue_native_admission_shard_view({mc, shard}, deadline);
  }

  static void complete_shared_admission_view(ExtMessagePool &pool, const BlockIdExt &mc,
                                             const BlockIdExt &shard, td::Result<ShardViewPtr> result) {
    pool.complete_native_admission_shared_shard_view({mc, shard}, std::move(result));
  }

  static std::string shared_admission_stats(const ExtMessagePool &pool) {
    return pool.native_batch_telemetry_string(':');
  }

  static std::size_t shared_admission_entries(const ExtMessagePool &pool) {
    return pool.native_admission_shard_waits_.size();
  }

  static std::size_t shared_admission_waiters(const ExtMessagePool &pool) {
    return pool.native_admission_shard_waiter_count_;
  }

  static void record_admission_manager_wait(ExtMessagePool &pool) {
    ++pool.native_batch_shard_manager_waits_;
  }

  static void record_admission_manager_wait_error(ExtMessagePool &pool, td::Status error) {
    pool.record_native_admission_manager_wait_error(error);
  }

  static bool admission_manager_wait_finished_after_deadline(ExtMessagePool &pool, td::Timestamp deadline) {
    return pool.native_admission_manager_wait_finished_after_deadline(deadline);
  }

  static std::string batch_admission_stats(ExtMessagePool &pool) {
    for (auto &[key, value] : pool.prepare_stats()) {
      if (key == "total.ext_msg_batch_admission") {
        return value;
      }
    }
    return {};
  }

  static bool observe_account_state(ExtMessagePool &pool, NativeAddress source, td::uint64 nonce, td::uint64 balance,
                                    LogicalTime lt) {
    return pool.native_nonce_watermarks_[source].observe_account_state(nonce, balance, lt);
  }

  static td::optional<td::uint64> first_unconsumed_nonce(const ExtMessagePool &pool, NativeAddress source) {
    return pool.native_nonce_watermarks_.at(source).first_unconsumed_nonce();
  }

  static td::uint64 watermark_revision(const ExtMessagePool &pool, NativeAddress source) {
    return pool.native_nonce_watermarks_.at(source).revision;
  }

  static ExtMessage::Hash add(ExtMessagePool &pool, NativeAddress source, td::uint64 nonce, int priority = 0,
                              bool active = true, bool committed = true, td::uint64 amount = 1,
                              td::uint64 fee = 0, bool link_direct = true) {
    auto hash = make_bits(static_cast<td::uint32>(nonce + 1), static_cast<unsigned>(source.second.as_array()[0] + 64));
    auto message = td::make_ref<FakeExtMessage>(source.second, hash);
    auto mempool_message = std::make_shared<ExtMessagePool::MempoolMsg>(message);
    mempool_message->native_nonce = nonce;
    mempool_message->native_nonce_count = 1;
    mempool_message->native_is_run = false;
    mempool_message->in_mempool = true;
    mempool_message->active = active;
    if (!active) {
      mempool_message->reactivate_at = td::Timestamp::in(60.0);
    }
    ExtMessagePool::MessageId id{message->shard(), hash};
    auto &messages = pool.ext_msgs_[priority];
    messages.ext_messages_ = messages.ext_messages_.insert(id, mempool_message);
    messages.native_messages_ =
        messages.native_messages_.insert(ExtMessagePool::NativeMessageId{nonce, id.dst, id.hash}, mempool_message);
    messages.ext_addr_messages_[source].emplace(hash, id);
    pool.ext_messages_hashes_[hash] = {priority, id};
    pool.ext_messages_hashes_norm_[hash].insert(ExtMessagePool::NormalizedMessageId{priority, id});

    auto &reservation = pool.native_accounts_[source].messages[nonce];
    reservation.hash = hash;
    reservation.source = source;
    reservation.logical_count = 1;
    reservation.is_run = false;
    reservation.amount = amount;
    reservation.fee = fee;
    reservation.valid_until = std::numeric_limits<td::uint32>::max();
    reservation.account_revision = pool.native_nonce_watermarks_[source].revision;
    reservation.committed = committed;
    if (link_direct) {
      reservation.set_mempool_link(mempool_message, priority, id);
    }
    return hash;
  }

  // Test-only scaffolding for a multi-transfer native work item. It builds one
  // physical pool object and one reservation keyed by the first nonce, which
  // is the representation used by admitted NTRN runs. Production NTFX
  // admission continues to call add() above with logical_count == 1.
  static ExtMessage::Hash add_work(ExtMessagePool &pool, NativeAddress source, td::uint64 first_nonce,
                                   td::uint32 logical_count, int priority = 0, bool active = true,
                                   bool committed = true, td::uint64 amount = 1, td::uint64 fee = 0,
                                   bool link_direct = true, bool is_run = true,
                                   td::uint32 payment_lane_depth = 0) {
    CHECK(logical_count != 0);
    CHECK(first_nonce <= std::numeric_limits<td::uint64>::max() - (logical_count - 1));
    auto hash =
        make_bits(static_cast<td::uint32>(first_nonce + 1), static_cast<unsigned>(source.second.as_array()[0] + 64));
    auto message = td::make_ref<FakeExtMessage>(source.second, hash);
    auto mempool_message = std::make_shared<ExtMessagePool::MempoolMsg>(message);
    mempool_message->native_nonce = first_nonce;
    mempool_message->native_nonce_count = logical_count;
    mempool_message->native_is_run = is_run;
    mempool_message->native_payment_lane_depth = payment_lane_depth;
    mempool_message->in_mempool = true;
    mempool_message->active = active;
    if (!active) {
      mempool_message->reactivate_at = td::Timestamp::in(60.0);
    }
    ExtMessagePool::MessageId id{message->shard(), hash};
    auto &messages = pool.ext_msgs_[priority];
    messages.ext_messages_ = messages.ext_messages_.insert(id, mempool_message);
    messages.native_messages_ = messages.native_messages_.insert(
        ExtMessagePool::NativeMessageId{first_nonce, id.dst, id.hash}, mempool_message);
    messages.ext_addr_messages_[source].emplace(hash, id);
    pool.ext_messages_hashes_[hash] = {priority, id};
    pool.ext_messages_hashes_norm_[hash].insert(ExtMessagePool::NormalizedMessageId{priority, id});

    auto &reservation = pool.native_accounts_[source].messages[first_nonce];
    reservation.hash = hash;
    reservation.source = source;
    reservation.logical_count = logical_count;
    reservation.is_run = is_run;
    reservation.payment_lane_depth = payment_lane_depth;
    reservation.amount = amount;
    reservation.fee = fee;
    reservation.valid_until = std::numeric_limits<td::uint32>::max();
    reservation.account_revision = pool.native_nonce_watermarks_[source].revision;
    reservation.committed = committed;
    if (link_direct) {
      reservation.set_mempool_link(mempool_message, priority, id);
    }
    return hash;
  }

  static void set_watermark(ExtMessagePool &pool, NativeAddress source, td::uint64 next_nonce) {
    auto &watermark = pool.native_nonce_watermarks_[source];
    watermark.observed_next_nonce = next_nonce;
    watermark.revision = std::max<td::uint64>(watermark.revision, 1);
  }

  static Selection select(ExtMessagePool &pool, ShardIdFull shard, std::size_t limit,
                          td::optional<NativeAddress> cursor = {}, std::vector<ExtMessage::Hash> excluded = {}) {
    std::sort(excluded.begin(), excluded.end());
    auto selected = pool.select_native_messages(shard, excluded, {}, limit, cursor);
    Selection result;
    result.cursor = selected.cursor;
    for (const auto &item : selected.items) {
      result.sources.push_back(item.source);
      result.nonces.push_back(item.nonce);
      result.logical_counts.push_back(item.logical_count);
    }
    result.scanned = selected.counters.scanned;
    result.selected = selected.counters.selected;
    result.logical_selected = selected.counters.logical_selected;
    result.inactive = selected.counters.inactive;
    result.excluded = selected.counters.excluded;
    result.ready_sources = selected.counters.ready_sources;
    result.head_gaps = selected.counters.head_gaps;
    result.head_missing_nonce = selected.counters.head_missing_nonce;
    result.runs = selected.counters.runs;
    result.max_run_size = selected.counters.max_run_size;
    return result;
  }

  static std::size_t make_due_and_reactivate(ExtMessagePool &pool, const ExtMessage::Hash &hash) {
    auto hash_it = pool.ext_messages_hashes_.find(hash);
    CHECK(hash_it != pool.ext_messages_hashes_.end());
    auto priority_it = pool.ext_msgs_.find(hash_it->second.first);
    CHECK(priority_it != pool.ext_msgs_.end());
    auto message = priority_it->second.ext_messages_.find(hash_it->second.second);
    CHECK(message);
    message.value()->active = false;
    message.value()->reactivate_at = td::Timestamp::now();
    pool.native_reactivations_.emplace(message.value()->reactivate_at, hash_it->second);
    return pool.reactivate_due_native_messages(td::Timestamp::in(0.01));
  }

  static void install_live_waiting_callback(ExtMessagePool &pool, std::size_t queue_capacity,
                                            std::size_t transport_message_capacity = 500,
                                            std::vector<ExtMessage::Hash> excluded = {},
                                            ShardIdFull shard = {basechainId, shardIdAll},
                                            NativeSourceNonceFloors native_source_nonce_floors = {}) {
    auto callback = std::make_unique<ExtMsgCallback>();
    callback->shard = shard;
    callback->queue_capacity = queue_capacity;
    callback->transport_message_capacity = transport_message_capacity;
    callback->timeout = td::Timestamp::in(60.0);
    callback->native_streaming = true;
    std::sort(excluded.begin(), excluded.end());
    excluded.erase(std::unique(excluded.begin(), excluded.end()), excluded.end());
    callback->excluded_messages = std::move(excluded);
    callback->native_source_nonce_floors = std::move(native_source_nonce_floors);
    auto installed = std::make_shared<ExtMessagePool::InstalledCallback>(std::move(callback));
    // Model the installed callback's serialized pump already waiting. This
    // keeps the unit test actor-free while ensuring the post-commit wake appends
    // work to the existing callback instead of creating another ingress event.
    installed->pump_active = true;
    installed->callback->queue_state->attach_telemetry(pool.native_transport_telemetry_);
    pool.callbacks_.push_back(std::move(installed));
  }

  static CallbackRound select_callback_round(ExtMessagePool &pool, ShardIdFull shard,
                                             std::size_t logical_limit,
                                             NativeSourceNonceFloors native_source_nonce_floors = {},
                                             std::vector<ExtMessage::Hash> excluded = {}) {
    auto callback = std::make_unique<ExtMsgCallback>();
    callback->shard = shard;
    callback->queue_capacity = logical_limit;
    callback->native_source_nonce_floors = std::move(native_source_nonce_floors);
    std::sort(excluded.begin(), excluded.end());
    callback->excluded_messages = std::move(excluded);
    auto installed = std::make_shared<ExtMessagePool::InstalledCallback>(std::move(callback));
    pool.restore_callback_native_cursor(installed);
    auto selection = pool.select_callback_native_messages(installed, logical_limit, logical_limit);
    installed->native_cursor = selection.cursor;
    pool.persist_callback_native_cursor(installed);

    CallbackRound result;
    result.cursor = selection.cursor;
    result.source_scans = selection.counters.source_scans;
    result.scanned = selection.counters.scanned;
    result.excluded = selection.counters.excluded;
    result.sources.reserve(selection.items.size());
    result.nonces.reserve(selection.items.size());
    result.logical_counts.reserve(selection.items.size());
    for (const auto &item : selection.items) {
      result.sources.push_back(item.source);
      result.nonces.push_back(item.nonce);
      result.logical_counts.push_back(item.logical_count);
    }
    return result;
  }

  static std::size_t callback_native_floor_sources(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.callbacks_.front()->callback->native_source_nonce_floors.size();
  }

  static bool finalize_existing_native(ExtMessagePool &pool, NativeAddress source, td::uint64 nonce,
                                       const ExtMessage::Hash &hash, int priority = 0) {
    auto hash_it = pool.ext_messages_hashes_.find(hash);
    CHECK(hash_it != pool.ext_messages_hashes_.end());
    auto message = pool.ext_msgs_[hash_it->second.first].ext_messages_.find(hash_it->second.second);
    CHECK(message);
    block::NativeTransfer transfer{.src = source.second,
                                   .dst = source.second,
                                   .amount = 1,
                                   .fee = 0,
                                   .nonce = nonce,
                                   .valid_until = std::numeric_limits<td::uint32>::max(),
                                   .signature = {}};
    auto [wait_allow_broadcast, allow_broadcast_promise] = td::actor::StartedTask<>::make_bridge();
    allow_broadcast_promise.set_value(td::Unit{});
    ExtMessagePool::CheckResult result{.message = message.value()->message,
                                       .wait_allow_broadcast = std::move(wait_allow_broadcast),
                                       .should_broadcast = true,
                                       .msg_seqno = {},
                                       .native_admission = ExtMessagePool::NativeAdmission{
                                           .first_nonce = transfer.nonce,
                                           .logical_count = 1,
                                           .amount = transfer.amount,
                                           .fee = transfer.fee,
                                           .valid_until = transfer.valid_until}};
    return pool.finalize_checked_message(std::move(result), priority, true, td::Timestamp::never()).is_ok();
  }

  static bool reservation_committed(const ExtMessagePool &pool, NativeAddress source, td::uint64 nonce) {
    return pool.native_accounts_.at(source).messages.at(nonce).committed;
  }

  static td::uint64 reservation_revision(const ExtMessagePool &pool, NativeAddress source, td::uint64 nonce) {
    return pool.native_accounts_.at(source).messages.at(nonce).account_revision;
  }

  static td::uint64 reserved_amount_before(const ExtMessagePool &pool, NativeAddress source, td::uint64 native_nonce,
                                           td::uint64 before_nonce) {
    return pool.native_accounts_.at(source).reserved_amount_before(native_nonce, before_nonce);
  }

  static void set_reservation_hash(ExtMessagePool &pool, NativeAddress source, td::uint64 nonce,
                                   ExtMessage::Hash hash) {
    pool.native_accounts_.at(source).messages.at(nonce).hash = hash;
  }

  static std::vector<td::uint64> reservation_nonces(const ExtMessagePool &pool, NativeAddress source) {
    std::vector<td::uint64> nonces;
    auto account = pool.native_accounts_.find(source);
    if (account == pool.native_accounts_.end()) {
      return nonces;
    }
    for (const auto &[nonce, _] : account->second.messages) {
      nonces.push_back(nonce);
    }
    return nonces;
  }

  static bool exact_retry_is_idempotent(ExtMessagePool &pool, const ExtMessage::Hash &hash) {
    auto hash_it = pool.ext_messages_hashes_.find(hash);
    CHECK(hash_it != pool.ext_messages_hashes_.end());
    auto priority = pool.ext_msgs_.find(hash_it->second.first);
    CHECK(priority != pool.ext_msgs_.end());
    auto message = priority->second.ext_messages_.find(hash_it->second.second);
    CHECK(message);
    auto result = pool.check_existing_external_message(message.value()->message, hash_it->second.first, true);
    CHECK(result.is_ok());
    auto existing = result.move_as_ok();
    return existing && !existing.value().should_broadcast;
  }

  static td::uint64 exact_retry_preserved_stale_revision(const ExtMessagePool &pool) {
    return pool.native_exact_retry_preserved_stale_revision_;
  }

  static td::uint64 rebased_reservations(const ExtMessagePool &pool) {
    return pool.native_reconciliation_rebased_reservations_;
  }

  static td::uint64 unaffordable_tail_pruned(const ExtMessagePool &pool) {
    return pool.native_reconciliation_unaffordable_tail_pruned_;
  }

  static td::uint64 stale_uncommitted_tail_pruned(const ExtMessagePool &pool) {
    return pool.native_reconciliation_stale_uncommitted_tail_pruned_;
  }

  static void set_valid_until(ExtMessagePool &pool, NativeAddress source, td::uint64 nonce,
                              td::uint32 valid_until) {
    pool.native_accounts_.at(source).messages.at(nonce).valid_until = valid_until;
  }

  static void set_native_observed_utime(ExtMessagePool &pool, NativeAddress source, UnixTime utime) {
    pool.native_accounts_.at(source).observed_utime = utime;
  }

  static td::uint64 expiry_suffix_events(const ExtMessagePool &pool) {
    return pool.native_expiry_suffix_events_;
  }

  static td::uint64 expiry_suffix_pruned(const ExtMessagePool &pool) {
    return pool.native_expiry_suffix_pruned_;
  }

  static void track_locally_accepted(
      ExtMessagePool &pool, std::vector<std::tuple<NativeAddress, td::uint64, ExtMessage::Hash>> messages) {
    std::vector<TrackedNativeExternalMessage> tracked;
    tracked.reserve(messages.size());
    for (const auto &[source, nonce, hash] : messages) {
      tracked.push_back(TrackedNativeExternalMessage{
          .hash = hash, .workchain = source.first, .source = source.second, .nonce = nonce});
    }
    pool.track_locally_accepted_native_messages(std::move(tracked));
  }

  static void track_locally_accepted_records(ExtMessagePool &pool, std::vector<TrackedNativeExternalMessage> messages) {
    pool.track_locally_accepted_native_messages(std::move(messages));
  }

  static bool contains(const ExtMessagePool &pool, const ExtMessage::Hash &hash) {
    return pool.ext_messages_hashes_.contains(hash);
  }

  static td::optional<td::uint64> tracked_nonce(const ExtMessagePool &pool, NativeAddress source) {
    auto it = pool.locally_accepted_native_nonces_.find(source);
    if (it == pool.locally_accepted_native_nonces_.end()) {
      return {};
    }
    return it->second;
  }

  static td::uint64 reconciliation_tracked_logical_messages(const ExtMessagePool &pool) {
    return pool.native_reconciliation_tracked_messages_;
  }

  static void register_pending_reconciliation_targets(ExtMessagePool &pool) {
    pool.register_pending_native_reconciliation_targets();
  }

  static bool should_reconcile_shard_top(ExtMessagePool &pool, const BlockIdExt &shard_top,
                                         std::size_t source_count) {
    return pool.should_reconcile_native_shard_top(shard_top, source_count);
  }

  static void record_successful_shard_reconciliation(ExtMessagePool &pool, const BlockIdExt &shard_top) {
    pool.record_successful_native_shard_reconciliation(shard_top);
  }

  static td::uint64 unchanged_top_skips(const ExtMessagePool &pool) {
    return pool.native_reconciliation_unchanged_top_skips_;
  }

  static td::uint64 unchanged_source_skips(const ExtMessagePool &pool) {
    return pool.native_reconciliation_unchanged_source_skips_;
  }

  static bool should_skip_state_fingerprint(ExtMessagePool &pool, const std::vector<BlockIdExt> &tops) {
    ExtMessagePool::NativeShardTopFingerprint fingerprint;
    for (const auto &top : tops) {
      fingerprint.emplace(top.shard_full(), top);
    }
    return pool.should_skip_native_reconciliation_state(fingerprint);
  }

  static void record_successful_state_fingerprint(ExtMessagePool &pool, const std::vector<BlockIdExt> &tops) {
    ExtMessagePool::NativeShardTopFingerprint fingerprint;
    for (const auto &top : tops) {
      fingerprint.emplace(top.shard_full(), top);
    }
    pool.record_successful_native_reconciliation_state(std::move(fingerprint));
  }

  static td::uint64 unchanged_state_skips(const ExtMessagePool &pool) {
    return pool.native_reconciliation_unchanged_state_skips_;
  }

  static void expire(ExtMessagePool &pool, const ExtMessage::Hash &hash) {
    auto hash_it = pool.ext_messages_hashes_.find(hash);
    CHECK(hash_it != pool.ext_messages_hashes_.end());
    auto priority_it = pool.ext_msgs_.find(hash_it->second.first);
    CHECK(priority_it != pool.ext_msgs_.end());
    auto message = priority_it->second.ext_messages_.find(hash_it->second.second);
    CHECK(message);
    message.value()->delete_at = td::Timestamp::now();
  }

  static bool reconcile_account(ExtMessagePool &pool, NativeAddress source, td::uint64 native_nonce,
                                td::uint64 balance = 1'000, UnixTime utime = 100, LogicalTime lt = 100) {
    auto result = pool.apply_canonical_native_account_state(source, native_nonce, balance, utime, lt);
    CHECK(result.is_ok());
    return result.ok();
  }

  static td::Result<bool> reconcile_account_with_telemetry(ExtMessagePool &pool, NativeAddress source,
                                                          td::uint64 native_nonce, td::uint64 balance,
                                                          UnixTime utime, LogicalTime lt) {
    return pool.apply_reconciled_native_account_state(source, native_nonce, balance, utime, lt);
  }

  static const NativeReconciliationTelemetry &reconciliation_telemetry(const ExtMessagePool &pool) {
    return pool.native_reconciliation_telemetry_;
  }

  static td::uint64 historical_sources_advanced(const ExtMessagePool &pool) {
    return pool.native_reconciliation_sources_advanced_;
  }

  static void set_reconciliation_profile(ExtMessagePool &pool, bool enabled) {
    pool.native_reconciliation_profile_enabled_ = enabled;
  }

  static std::string reconciliation_diagnostics(ExtMessagePool &pool) {
    for (auto &[key, value] : pool.prepare_stats()) {
      if (key == "total.ext_msg_native_reconciliation_diagnostics") {
        return value;
      }
    }
    return {};
  }

  static bool callback_has_delivery(const ExtMessagePool &pool, const ExtMessage::Hash &hash) {
    return pool.callbacks_.size() == 1 && pool.callbacks_.front()->delivered_native.contains(hash) &&
           pool.callbacks_.front()->pending_native.size() == 1 &&
           pool.callbacks_.front()->pending_native.front().message &&
           pool.callbacks_.front()->pending_native.front().message->first->hash() == hash;
  }

  static bool callback_contains_delivery(const ExtMessagePool &pool, const ExtMessage::Hash &hash) {
    if (pool.callbacks_.size() != 1 || !pool.callbacks_.front()->delivered_native.contains(hash)) {
      return false;
    }
    return std::any_of(pool.callbacks_.front()->pending_native.begin(),
                       pool.callbacks_.front()->pending_native.end(), [&](const auto &entry) {
                         return entry.message && entry.message->first->hash() == hash;
                       });
  }

  static std::vector<ExtMessage::Hash> callback_delivery_hashes(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    std::vector<ExtMessage::Hash> hashes;
    for (const auto &entry : pool.callbacks_.front()->pending_native) {
      CHECK(entry.message);
      hashes.push_back(entry.message->first->hash());
    }
    return hashes;
  }

  static std::vector<td::uint32> callback_delivery_logical_counts(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    std::vector<td::uint32> counts;
    for (const auto &entry : pool.callbacks_.front()->pending_native) {
      counts.push_back(entry.logical_native_count());
    }
    return counts;
  }

  static void clear_native_mempool_link(ExtMessagePool &pool, NativeAddress source, td::uint64 nonce) {
    pool.native_accounts_.at(source).messages.at(nonce).clear_mempool_link();
  }

  static void mark_native_mempool_link_stale(ExtMessagePool &pool, NativeAddress source, td::uint64 nonce) {
    auto &link = pool.native_accounts_.at(source).messages.at(nonce).mempool_link;
    CHECK(link);
    link.value().message->in_mempool = false;
  }

  static bool has_native_mempool_link(const ExtMessagePool &pool, NativeAddress source, td::uint64 nonce) {
    auto account = pool.native_accounts_.find(source);
    return account != pool.native_accounts_.end() && account->second.messages.contains(nonce) &&
           account->second.messages.at(nonce).mempool_link;
  }

  static bool has_native_reservation(const ExtMessagePool &pool, NativeAddress source, td::uint64 nonce) {
    auto account = pool.native_accounts_.find(source);
    return account != pool.native_accounts_.end() && account->second.messages.contains(nonce);
  }

  static std::weak_ptr<ExtMessagePool::MempoolMsg> native_mempool_weak(ExtMessagePool &pool,
                                                                         NativeAddress source, td::uint64 nonce) {
    auto &link = pool.native_accounts_.at(source).messages.at(nonce).mempool_link;
    CHECK(link);
    return link.value().message;
  }

  static void cancel_callback(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    pool.cancel_callback_delivery(pool.callbacks_.front());
  }

  static std::size_t fill_once(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    td::actor::core::ActorExecuteContext context(&pool);
    td::actor::core::ActorExecuteContext::Guard guard(&context);
    auto callback = pool.callbacks_.front();
    pool.begin_callback_epoch(callback);
    return pool.fill_callback_native(callback, false);
  }

  static std::size_t prefill_native_transport(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    td::actor::core::ActorExecuteContext context(&pool);
    td::actor::core::ActorExecuteContext::Guard guard(&context);
    auto callback = pool.callbacks_.front();
    pool.begin_callback_epoch(callback);
    return pool.prefill_callback_native(callback, true);
  }

  static std::size_t prefill_native_transport_low_watermark(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    td::actor::core::ActorExecuteContext context(&pool);
    td::actor::core::ActorExecuteContext::Guard guard(&context);
    return pool.prefill_callback_native_low_watermark(pool.callbacks_.front());
  }

  static std::size_t callback_native_transport_publish_batch_capacity(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.native_transport_publish_batch_capacity(*pool.callbacks_.front());
  }

  static void consume_callback_native(ExtMessagePool &pool, std::size_t consumed) {
    CHECK(pool.callbacks_.size() == 1);
    pool.callbacks_.front()->callback->queue_state->record_consumed(consumed);
  }

  static void publish_callback_pending_native(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    auto callback = pool.callbacks_.front();
    std::size_t logical = 0;
    for (const auto &entry : callback->pending_native) {
      logical += entry.logical_native_count();
    }
    const auto published = callback->pending_native.size();
    CHECK(published != 0);
    if (callback->initial_native_publish_pending != 0) {
      CHECK(published <= callback->initial_native_publish_pending);
      callback->initial_native_publish_pending -= published;
    }
    callback->pending_native.clear();
    callback->callback->queue_state->record_pushed(published, logical);
  }

  static std::shared_ptr<ExtMsgQueueTelemetry> callback_transport_telemetry(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.native_transport_telemetry_;
  }

  static td::uint64 callback_selected_ahead(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.callbacks_.front()->callback->queue_state->native_selected_ahead();
  }

  static td::uint64 callback_logical_selected_ahead(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.callbacks_.front()->callback->queue_state->native_logical_selected_ahead();
  }

  static std::size_t callback_native_transport_selected_limit(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.native_transport_selected_limit(*pool.callbacks_.front());
  }

  static td::uint32 native_source_run_target() {
    return static_cast<td::uint32>(ExtMessagePool::NATIVE_SOURCE_RUN_TARGET);
  }

  static bool callback_native_transport_has_refill_credit(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.native_transport_has_refill_credit(*pool.callbacks_.front());
  }

  static void record_callback_selected(ExtMessagePool &pool, std::size_t count) {
    CHECK(pool.callbacks_.size() == 1);
    pool.callbacks_.front()->callback->queue_state->record_selected(count);
  }

  static std::size_t fill_sources(ExtMessagePool &pool, const std::set<NativeAddress> &sources) {
    CHECK(pool.callbacks_.size() == 1);
    td::actor::core::ActorExecuteContext context(&pool);
    td::actor::core::ActorExecuteContext::Guard guard(&context);
    auto callback = pool.callbacks_.front();
    pool.begin_callback_epoch(callback);
    return pool.fill_callback_native(callback, false, &sources);
  }

  static SchedulerStats scheduler_stats(const ExtMessagePool &pool) {
    const auto &stats = pool.native_queue_counters_;
    return SchedulerStats{.selected = stats.selected,
                          .direct_link_hits = stats.direct_link_hits,
                          .direct_link_fallbacks = stats.direct_link_fallbacks,
                          .builds = stats.scheduler_builds,
                          .source_scans = stats.source_scans,
                          .source_refreshes = stats.source_refreshes,
                          .source_probes = stats.source_probes,
                          .runs = stats.runs,
                          .inactive = stats.inactive,
                          .excluded = stats.excluded,
                          .head_gaps = stats.head_gaps};
  }

  static std::size_t callback_pending(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.callbacks_.front()->pending_native.size() + pool.callbacks_.front()->pending_generic.size();
  }

  static std::size_t wake_sources(ExtMessagePool &pool, const std::set<NativeAddress> &sources,
                                  bool preserve_valid_ready_head = false) {
    return pool.wake_native_callbacks(&sources, preserve_valid_ready_head);
  }

  static std::size_t dirty_sources(const ExtMessagePool &pool, std::size_t callback_index = 0) {
    CHECK(callback_index < pool.callbacks_.size());
    return pool.callbacks_[callback_index]->native_dirty_sources.size();
  }

  static bool has_dirty_source(const ExtMessagePool &pool, std::size_t callback_index,
                               const NativeAddress &source) {
    CHECK(callback_index < pool.callbacks_.size());
    return pool.callbacks_[callback_index]->native_dirty_sources.contains(source);
  }

  static std::size_t resume_native_pump_refill(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    td::actor::core::ActorExecuteContext context(&pool);
    td::actor::core::ActorExecuteContext::Guard guard(&context);
    auto callback = pool.callbacks_.front();
    auto dirty_sources = std::move(callback->native_dirty_sources);
    callback->native_dirty_sources.clear();
    if (callback->native_scheduler_rebuild) {
      callback->native_scheduler = {};
      callback->native_scheduler_rebuild = false;
      dirty_sources.clear();
    }
    return pool.fill_callback_native(callback, false, dirty_sources.empty() ? nullptr : &dirty_sources);
  }

  static constexpr std::size_t native_delivery_chunk() {
    return ExtMessagePool::NATIVE_DELIVERY_CHUNK;
  }

  static constexpr std::size_t max_native_queue_limit() {
    return ExtMessagePool::MAX_NATIVE_COLLATOR_QUEUE_LIMIT;
  }

  static bool native_transfer_runs_enabled(int global_version, bool has_capabilities, long long capabilities) {
    return ExtMessagePool::native_transfer_runs_enabled(global_version, has_capabilities, capabilities);
  }

  static td::Status validate_native_admission_mode(ExtMessagePool &pool, bool runs_enabled, bool is_run) {
    // This uses the same stateful mode transition and gate as both production
    // admission paths. In particular, cleanup happens before the caller can
    // ask check_existing_external_message() for a raw-hash retry.
    pool.update_native_transfer_runs_mode(runs_enabled);
    ExtMessagePool::NativeAdmissionSnapshot snapshot{};
    snapshot.runs_enabled = runs_enabled;
    return pool.validate_native_admission_mode(snapshot, is_run);
  }

  static void update_native_transfer_runs_mode(ExtMessagePool &pool, bool runs_enabled) {
    pool.update_native_transfer_runs_mode(runs_enabled);
  }

  static void update_native_payment_lane_depth(ExtMessagePool &pool,
                                               td::optional<td::uint32> payment_lane_depth) {
    pool.update_native_payment_lane_depth(std::move(payment_lane_depth));
  }

  static bool has_exact_native_retry(ExtMessagePool &pool, NativeAddress source, const ExtMessage::Hash &hash) {
    auto message = td::make_ref<FakeExtMessage>(source.second, hash);
    auto existing = pool.check_existing_external_message(std::move(message), 0, true);
    CHECK(existing.is_ok());
    return static_cast<bool>(existing.ok());
  }

  static td::Result<ExtMessagePool::NativeAdmission> native_run_admission(
      const block::NativeTransferRun &run) {
    return ExtMessagePool::make_native_admission(run);
  }
};

static_assert(ExtMessagePoolTestAccess::max_native_queue_limit() == 65'536);
static_assert(ExtMessagePoolTestAccess::max_native_queue_limit() == block::NativeTransferBatch::max_entries);
static_assert(ExtMessagePoolTestAccess::native_delivery_chunk() == 512);

TEST(ExtMessagePoolScheduler, MalformedConfigurationDoesNotPopulateAdmissionCache) {
  ASSERT_TRUE(ExtMessagePoolTestAccess::malformed_config_is_never_cached());
}

TEST(ExtMessagePoolScheduler, NativeTransferRunAdmissionRequiresVersionAndCapability) {
  ASSERT_TRUE(!ExtMessagePoolTestAccess::native_transfer_runs_enabled(
      block::NativeTransferBatch::runs_global_version - 1, true, ton::capNativeTransferRuns));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::native_transfer_runs_enabled(
      block::NativeTransferBatch::runs_global_version, false, ton::capNativeTransferRuns));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::native_transfer_runs_enabled(
      block::NativeTransferBatch::runs_global_version, true, 0));
  ASSERT_TRUE(ExtMessagePoolTestAccess::native_transfer_runs_enabled(
      block::NativeTransferBatch::runs_global_version, true, ton::capNativeTransferRuns));
}

TEST(ExtMessagePoolScheduler, NativeTransferRunAdmissionUsesOneAtomicAggregateInterval) {
  block::NativeTransferRun run;
  run.src = make_bits(7, 19);
  run.first_nonce = 41;
  run.valid_until = std::numeric_limits<UnixTime>::max();
  run.signature.assign(64, '\x01');
  run.outputs = {{.dst = make_bits(8, 20), .amount = 5, .fee = 2},
                 {.dst = make_bits(9, 21), .amount = 7, .fee = 3}};

  auto admission = ExtMessagePoolTestAccess::native_run_admission(run);
  ASSERT_TRUE(admission.is_ok());
  auto value = admission.move_as_ok();
  ASSERT_EQ(value.first_nonce, 41u);
  ASSERT_EQ(value.logical_count, 2u);
  ASSERT_EQ(value.last_nonce(), td::optional<td::uint64>(42));
  ASSERT_EQ(value.amount, 12u);
  ASSERT_EQ(value.fee, 5u);
  ASSERT_EQ(value.required_amount(), td::optional<td::uint64>(17));

  // Each output is individually valid, but aggregate debit must never wrap
  // when the pool reserves the signed work as a single interval.
  run.outputs = {{.dst = make_bits(8, 20), .amount = std::numeric_limits<td::uint64>::max(), .fee = 0},
                 {.dst = make_bits(9, 21), .amount = 1, .fee = 0}};
  ASSERT_TRUE(ExtMessagePoolTestAccess::native_run_admission(run).is_error());
}

TEST(ExtMessagePoolScheduler, NativeRunModeRejectsIncompatibleWireTypesBeforeExistingMessageLookup) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(108);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 10);
  auto scalar_hash = ExtMessagePoolTestAccess::add(pool, source, 10);
  ASSERT_TRUE(ExtMessagePoolTestAccess::has_exact_native_retry(pool, source, scalar_hash));

  // This is the stateful early gate used before check_existing_external_message
  // on the single-message path. Entering run-only mode must both reject the
  // scalar wire type and remove an old scalar exact-retry target first.
  auto scalar_in_run_mode =
      ExtMessagePoolTestAccess::validate_native_admission_mode(pool, true, false);
  ASSERT_TRUE(scalar_in_run_mode.is_error());
  ASSERT_TRUE(scalar_in_run_mode.error().message().str().find("scalar native transfers are disabled") !=
              std::string::npos);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, scalar_hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_native_reservation(pool, source, 10));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_exact_native_retry(pool, source, scalar_hash));

  // Conversely, a source-signed run never reaches the legacy path while the
  // capability/version gate is disabled.
  auto run_without_cap =
      ExtMessagePoolTestAccess::validate_native_admission_mode(pool, false, true);
  ASSERT_TRUE(run_without_cap.is_error());
  ASSERT_TRUE(run_without_cap.error().message().str().find("capNativeTransferRuns") != std::string::npos);
}

TEST(ExtMessagePoolScheduler, NativeRunModeTransitionsPurgeOnlyIncompatibleReservations) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto scalar_source = ExtMessagePoolTestAccess::source(109);
  auto run_source = ExtMessagePoolTestAccess::source(110);
  ExtMessagePoolTestAccess::set_watermark(pool, scalar_source, 20);
  ExtMessagePoolTestAccess::set_watermark(pool, run_source, 30);
  auto scalar_hash = ExtMessagePoolTestAccess::add(pool, scalar_source, 20);
  auto run_hash = ExtMessagePoolTestAccess::add_work(pool, run_source, 30, 2, 0, true, true, 7, 3, true, true);

  ExtMessagePoolTestAccess::update_native_transfer_runs_mode(pool, true);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, scalar_hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_native_reservation(pool, scalar_source, 20));
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, run_hash));
  ASSERT_TRUE(ExtMessagePoolTestAccess::has_native_reservation(pool, run_source, 30));

  // The rollback/reorg direction is symmetric: the old v5 parent BOC and its
  // complete reservation disappear before a scalar head can be admitted.
  ExtMessagePoolTestAccess::update_native_transfer_runs_mode(pool, false);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, run_hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_native_reservation(pool, run_source, 30));
}

TEST(ExtMessagePoolScheduler, NativePaymentLaneActivationPurgesOldRunReservations) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto old_lane_source = ExtMessagePoolTestAccess::source(117);
  auto current_lane_source = ExtMessagePoolTestAccess::source(118);
  ExtMessagePoolTestAccess::set_watermark(pool, old_lane_source, 50);
  ExtMessagePoolTestAccess::set_watermark(pool, current_lane_source, 60);
  ExtMessagePoolTestAccess::update_native_transfer_runs_mode(pool, true);

  // A v5 reservation has no lane-depth tag. As soon as a v16 fixed-lane
  // policy is installed, the exact hash and the nonce reservation must both
  // disappear before a retry can observe them.
  auto old_hash =
      ExtMessagePoolTestAccess::add_work(pool, old_lane_source, 50, 2, 0, true, true, 7, 3, true, true);
  ExtMessagePoolTestAccess::update_native_payment_lane_depth(pool, 1);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, old_hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_native_reservation(pool, old_lane_source, 50));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_exact_native_retry(pool, old_lane_source, old_hash));

  // A reservation admitted under the current depth survives a refresh of the
  // same policy, but a depth change clears it atomically as well.
  auto current_hash =
      ExtMessagePoolTestAccess::add_work(pool, current_lane_source, 60, 2, 0, true, true, 11, 5, true, true, 1);
  ExtMessagePoolTestAccess::update_native_payment_lane_depth(pool, 1);
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, current_hash));
  ASSERT_TRUE(ExtMessagePoolTestAccess::has_native_reservation(pool, current_lane_source, 60));

  ExtMessagePoolTestAccess::update_native_payment_lane_depth(pool, 2);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, current_hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_native_reservation(pool, current_lane_source, 60));
}

TEST(ExtMessagePoolScheduler, NativeRunReservationKeepsAggregateDebitAndIntervalAtomic) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(111);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 40);
  ExtMessagePoolTestAccess::update_native_transfer_runs_mode(pool, true);
  auto hash = ExtMessagePoolTestAccess::add_work(pool, source, 40, 3, 0, true, true, 12, 5, true, true);

  // One physical NTRN owns all three logical nonces and reserves its full
  // aggregate amount + fees. There is no synthetic child at nonce 41 or 42.
  ASSERT_EQ(ExtMessagePoolTestAccess::reservation_nonces(pool, source), std::vector<td::uint64>({40}));
  ASSERT_EQ(ExtMessagePoolTestAccess::reserved_amount_before(pool, source, 40, 43), 17u);

  auto too_small = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 2);
  ASSERT_EQ(too_small.selected, 0u);
  ASSERT_EQ(too_small.logical_selected, 0u);

  auto fitting = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 3);
  ASSERT_EQ(fitting.selected, 1u);
  ASSERT_EQ(fitting.logical_selected, 3u);
  ASSERT_EQ(fitting.nonces, std::vector<td::uint64>({40}));
  ASSERT_EQ(fitting.logical_counts, std::vector<td::uint32>({3}));
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, hash));
}

TEST(ExtMessagePoolScheduler, NativeAdmissionCacheUsesExactShardBlockId) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto mc_state = ExtMessagePoolTestAccess::masterchain_state(100);
  auto mc_block_id = mc_state->get_block_id();
  auto shard_block_id = ExtMessagePoolTestAccess::shard_top(10);
  pool.update_last_masterchain_state(mc_state);

  // The public state-update path must establish the first cache generation.
  // Otherwise this successful first fill would be suppressed as stale and the
  // second exact request could not hit.
  ASSERT_TRUE(!ExtMessagePoolTestAccess::lookup_admission_view(pool, mc_block_id, shard_block_id));
  auto first_fill = ExtMessagePoolTestAccess::store_admission_view(
      pool, mc_block_id, ExtMessagePoolTestAccess::admission_view(shard_block_id, 1));
  ASSERT_TRUE(first_fill.is_ok());
  auto first_view = first_fill.move_as_ok();
  auto exact_hit = ExtMessagePoolTestAccess::lookup_admission_view(pool, mc_block_id, shard_block_id);
  ASSERT_TRUE(exact_hit);
  ASSERT_EQ(exact_hit.get(), first_view.get());

  // A same-height shard replacement has the same short BlockId but different
  // root/file hashes. It is an exact-ID miss and occupies a distinct entry.
  auto shard_fork = ExtMessagePoolTestAccess::shard_top(10, 1);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::lookup_admission_view(pool, mc_block_id, shard_fork));
  auto fork_fill = ExtMessagePoolTestAccess::store_admission_view(
      pool, mc_block_id, ExtMessagePoolTestAccess::admission_view(shard_fork, 2));
  ASSERT_TRUE(fork_fill.is_ok());

  auto stats = ExtMessagePoolTestAccess::admission_cache_stats(pool);
  ASSERT_EQ(stats.requests, 3u);
  ASSERT_EQ(stats.hits, 1u);
  ASSERT_EQ(stats.fills, 2u);
  ASSERT_EQ(stats.entries, 2u);
  ASSERT_EQ(stats.peak_entries, 2u);
}

TEST(ExtMessagePoolScheduler, NativeAdmissionCacheResetsOnExactMasterchainGeneration) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto mc_state = ExtMessagePoolTestAccess::masterchain_state(100);
  auto mc_block_id = mc_state->get_block_id();
  auto shard_block_id = ExtMessagePoolTestAccess::shard_top(10);
  pool.update_last_masterchain_state(mc_state);
  ASSERT_TRUE(ExtMessagePoolTestAccess::store_admission_view(
                  pool, mc_block_id, ExtMessagePoolTestAccess::admission_view(shard_block_id, 1))
                  .is_ok());

  // Re-observing the exact generation is idempotent and retains its entries.
  ExtMessagePoolTestAccess::reset_admission_generation(pool, mc_block_id);
  auto stats = ExtMessagePoolTestAccess::admission_cache_stats(pool);
  ASSERT_EQ(stats.generation_resets, 0u);
  ASSERT_EQ(stats.entries, 1u);

  // Even a same-height MC fork is a different exact generation. This direct
  // cache-policy exercise is independent of the actor's stale-update filter.
  auto mc_fork = ExtMessagePoolTestAccess::masterchain_state(100, 1)->get_block_id();
  ExtMessagePoolTestAccess::reset_admission_generation(pool, mc_fork);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::lookup_admission_view(pool, mc_fork, shard_block_id));
  stats = ExtMessagePoolTestAccess::admission_cache_stats(pool);
  ASSERT_EQ(stats.generation_resets, 1u);
  ASSERT_EQ(stats.entries, 0u);
  ASSERT_EQ(stats.peak_entries, 1u);

  ASSERT_TRUE(ExtMessagePoolTestAccess::store_admission_view(
                  pool, mc_fork, ExtMessagePoolTestAccess::admission_view(shard_block_id, 2))
                  .is_ok());
  pool.update_last_masterchain_state(ExtMessagePoolTestAccess::masterchain_state(101));
  stats = ExtMessagePoolTestAccess::admission_cache_stats(pool);
  ASSERT_EQ(stats.generation_resets, 2u);
  ASSERT_EQ(stats.entries, 0u);
}

TEST(ExtMessagePoolScheduler, NativeAdmissionCacheSuppressesStaleSuspendedFill) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto old_mc = ExtMessagePoolTestAccess::masterchain_state(100)->get_block_id();
  auto new_mc = ExtMessagePoolTestAccess::masterchain_state(101)->get_block_id();
  auto shard_block_id = ExtMessagePoolTestAccess::shard_top(10);
  ExtMessagePoolTestAccess::reset_admission_generation(pool, old_mc);
  auto suspended_view = ExtMessagePoolTestAccess::admission_view(shard_block_id, 1);

  // Model an actor coroutine resuming after a newer MC notification ran while
  // its manager read was suspended. The old pinned view remains valid for its
  // caller, but it must not populate the new generation.
  ExtMessagePoolTestAccess::reset_admission_generation(pool, new_mc);
  auto stale_fill = ExtMessagePoolTestAccess::store_admission_view(pool, old_mc, suspended_view);
  ASSERT_TRUE(stale_fill.is_ok());
  ASSERT_EQ(stale_fill.move_as_ok().get(), suspended_view.get());
  auto stats = ExtMessagePoolTestAccess::admission_cache_stats(pool);
  ASSERT_EQ(stats.stale_generation_fill_skips, 1u);
  ASSERT_EQ(stats.fills, 0u);
  ASSERT_EQ(stats.entries, 0u);

  ASSERT_TRUE(ExtMessagePoolTestAccess::store_admission_view(pool, new_mc, suspended_view).is_ok());
  stats = ExtMessagePoolTestAccess::admission_cache_stats(pool);
  ASSERT_EQ(stats.fills, 1u);
  ASSERT_EQ(stats.entries, 1u);
}

TEST(ExtMessagePoolScheduler, NativeAdmissionCacheFillRaceReusesRootAndRejectsConflict) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto mc_block_id = ExtMessagePoolTestAccess::masterchain_state(100)->get_block_id();
  auto shard_block_id = ExtMessagePoolTestAccess::shard_top(10);
  ExtMessagePoolTestAccess::reset_admission_generation(pool, mc_block_id);

  auto first_fill = ExtMessagePoolTestAccess::store_admission_view(
      pool, mc_block_id, ExtMessagePoolTestAccess::admission_view(shard_block_id, 1, 100, 200));
  ASSERT_TRUE(first_fill.is_ok());
  auto first_view = first_fill.move_as_ok();
  auto same_root_fill = ExtMessagePoolTestAccess::store_admission_view(
      pool, mc_block_id, ExtMessagePoolTestAccess::admission_view(shard_block_id, 1, 101, 201));
  ASSERT_TRUE(same_root_fill.is_ok());
  ASSERT_EQ(same_root_fill.move_as_ok().get(), first_view.get());

  auto conflicting_fill = ExtMessagePoolTestAccess::store_admission_view(
      pool, mc_block_id, ExtMessagePoolTestAccess::admission_view(shard_block_id, 2));
  ASSERT_TRUE(conflicting_fill.is_error());
  auto cached = ExtMessagePoolTestAccess::lookup_admission_view(pool, mc_block_id, shard_block_id);
  ASSERT_TRUE(cached);
  ASSERT_EQ(cached.get(), first_view.get());

  auto stats = ExtMessagePoolTestAccess::admission_cache_stats(pool);
  ASSERT_EQ(stats.fills, 1u);
  ASSERT_EQ(stats.fill_races, 2u);
  ASSERT_EQ(stats.fill_conflicts, 1u);
  ASSERT_EQ(stats.entries, 1u);
}

TEST(ExtMessagePoolScheduler, NativeAdmissionCacheValidationErrorsNeverInsert) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto mc_block_id = ExtMessagePoolTestAccess::masterchain_state(100)->get_block_id();
  auto requested_id = ExtMessagePoolTestAccess::shard_top(10);
  ExtMessagePoolTestAccess::reset_admission_generation(pool, mc_block_id);

  td::Ref<ShardState> wrong_state =
      td::make_ref<ShardStateQ>(ExtMessagePoolTestAccess::shard_top(10, 1), td::Ref<vm::Cell>{});
  ASSERT_TRUE(ExtMessagePoolTestAccess::make_admission_view(pool, requested_id, std::move(wrong_state)).is_error());
  auto stats = ExtMessagePoolTestAccess::admission_cache_stats(pool);
  ASSERT_EQ(stats.wrong_id, 1u);
  ASSERT_EQ(stats.invalid_header, 0u);
  ASSERT_EQ(stats.entries, 0u);

  td::Ref<ShardState> invalid_state = td::make_ref<ShardStateQ>(requested_id, td::Ref<vm::Cell>{});
  ASSERT_TRUE(ExtMessagePoolTestAccess::make_admission_view(pool, requested_id, std::move(invalid_state)).is_error());
  stats = ExtMessagePoolTestAccess::admission_cache_stats(pool);
  ASSERT_EQ(stats.wrong_id, 1u);
  ASSERT_EQ(stats.invalid_header, 1u);
  ASSERT_EQ(stats.entries, 0u);
}

TEST(ExtMessagePoolScheduler, NativeAdmissionManagerWaitTelemetrySeparatesDeadlineAndErrorOutcomes) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto record_error = [&](td::Status error) {
    ExtMessagePoolTestAccess::record_admission_manager_wait(pool);
    ExtMessagePoolTestAccess::record_admission_manager_wait_error(pool, std::move(error));
  };
  record_error(td::Status::Error(ErrorCode::timeout, "manager waiter timeout"));
  record_error(td::Status::Error(td::actor::AWAIT_TIMEOUT_CODE, "outer await timeout"));
  record_error(td::Status::Error(ErrorCode::notready, "state not ready"));
  record_error(td::Status::Error(ErrorCode::cancelled, "manager stopped"));

  // A manager success is still a logical manager wait. It need not be a
  // physical state fetch because the manager owns exact-ID joins and caching.
  ExtMessagePoolTestAccess::record_admission_manager_wait(pool);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::admission_manager_wait_finished_after_deadline(
      pool, td::Timestamp::in(1.0)));

  // A result that wins the timeout race after the caller's exact deadline is
  // rejected separately, before header validation or cache insertion.
  ASSERT_TRUE(ExtMessagePoolTestAccess::admission_manager_wait_finished_after_deadline(
      pool, td::Timestamp::at(td::Timestamp::now().at() - 1.0)));

  auto stats = ExtMessagePoolTestAccess::admission_cache_stats(pool);
  ASSERT_EQ(stats.manager_waits, 5u);
  ASSERT_EQ(stats.miss_errors, 4u);
  ASSERT_EQ(stats.manager_wait_errors, 4u);
  ASSERT_EQ(stats.manager_wait_timeouts, 2u);
  ASSERT_EQ(stats.manager_wait_notready, 1u);
  ASSERT_EQ(stats.manager_wait_other_errors, 1u);
  ASSERT_EQ(stats.manager_wait_late_results, 1u);
  ASSERT_EQ(stats.entries, 0u);

  auto emitted = ExtMessagePoolTestAccess::batch_admission_stats(pool);
  ASSERT_TRUE(emitted.find("shard_manager_waits:5") != std::string::npos);
  ASSERT_TRUE(emitted.find("shard_fetches:5") != std::string::npos);
  ASSERT_TRUE(emitted.find("shard_miss_errors:4") != std::string::npos);
  ASSERT_TRUE(emitted.find("shard_fetch_errors:4") != std::string::npos);
  ASSERT_TRUE(emitted.find("shard_manager_wait_errors:4") != std::string::npos);
  ASSERT_TRUE(emitted.find("shard_manager_wait_timeouts:2") != std::string::npos);
  ASSERT_TRUE(emitted.find("shard_manager_wait_notready:1") != std::string::npos);
  ASSERT_TRUE(emitted.find("shard_manager_wait_other_errors:1") != std::string::npos);
  ASSERT_TRUE(emitted.find("shard_manager_wait_late_results:1") != std::string::npos);
}

namespace {

struct SharedAdmissionManagerRequests {
  struct Request {
    BlockIdExt block;
    td::Timestamp deadline;
    td::Promise<td::Ref<ShardState>> promise;
  };
  std::vector<Request> requests;
};

class SharedAdmissionTestManager final : public ValidatorManagerImpl {
 public:
  explicit SharedAdmissionTestManager(std::shared_ptr<SharedAdmissionManagerRequests> requests)
      : ValidatorManagerImpl({}, "", {}, {}, {}, {}, {}), requests_(std::move(requests)) {
  }
  void start_up() override {
  }
  void wait_block_state_short(BlockIdExt block, td::uint32, td::Timestamp deadline, bool,
                              td::Promise<td::Ref<ShardState>> promise) override {
    requests_->requests.push_back({block, deadline, std::move(promise)});
  }

 private:
  std::shared_ptr<SharedAdmissionManagerRequests> requests_;
};

class SharedAdmissionTestState final : public ShardStateQ {
 public:
  explicit SharedAdmissionTestState(const BlockIdExt &id) : ShardStateQ(id, make_root(id)) {
  }
  RootHash root_hash() const override {
    return RootHash{root_cell()->get_hash().bits()};
  }

 private:
  static td::Ref<vm::Cell> make_root(const BlockIdExt &id) {
    // A minimally encoded unsplit basechain header with empty currency,
    // libraries and master-ref auxiliary fields. The admission projection
    // validates this actual TLB header rather than a preconstructed view.
    auto empty = vm::CellBuilder{}.finalize_novm();
    auto auxiliary = vm::CellBuilder{}.store_zeroes(140).finalize_novm();
    return vm::CellBuilder{}.store_long(0x9023afe2, 32).store_long(0, 32)
        .store_long(0, 8).store_long(basechainId, 32).store_long(0, 64)
        .store_long(id.seqno(), 32).store_long(0, 32).store_long(100, 32)
        .store_long(200, 64).store_long(0, 32).store_ref(empty)
        .store_long(0, 1).store_ref(empty).store_ref(auxiliary).store_long(0, 1).finalize_novm();
  }
};

class SharedAdmissionTestPool final : public ExtMessagePool {
 public:
  SharedAdmissionTestPool(td::actor::ActorId<ValidatorManager> manager, bool enabled,
                          std::shared_ptr<bool> destroyed = {})
      : ExtMessagePool({}, manager), enabled_(enabled), destroyed_(std::move(destroyed)) {
  }
  ~SharedAdmissionTestPool() override {
    if (destroyed_) {
      *destroyed_ = true;
    }
  }
  void start_up() override {
    ExtMessagePoolTestAccess::enable_shared_admission(*this, enabled_);
  }
  td::actor::Task<ExtMessagePoolTestAccess::ShardViewPtr> get(BlockIdExt mc, BlockIdExt shard,
                                                            td::Timestamp deadline) {
    co_return co_await ExtMessagePoolTestAccess::wait_admission_view(*this, mc, shard, deadline);
  }
  std::string diagnostics() {
    return ExtMessagePoolTestAccess::shared_admission_stats(*this);
  }
  void initialize_cache(BlockIdExt mc) {
    ExtMessagePoolTestAccess::reset_admission_generation(*this, mc);
  }
  std::string cache_diagnostics() {
    return ExtMessagePoolTestAccess::batch_admission_stats(*this);
  }
  void shutdown() {
    stop();
  }

 private:
  bool enabled_;
  std::shared_ptr<bool> destroyed_;
};

}  // namespace

TEST(ExtMessagePoolScheduler, SharedAdmissionFetchCoroutineDispatchesOnceAndFansOutManagerFailure) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto requests = std::make_shared<SharedAdmissionManagerRequests>();
    auto manager = td::actor::create_actor<SharedAdmissionTestManager>("shared-admission-manager", requests);
    auto pool = td::actor::create_actor<SharedAdmissionTestPool>("shared-admission-pool", manager.get(), true);
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    auto first = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(5));
    auto joined = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(8));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->requests.size(), 1u);
    requests->requests[0].promise.set_error(td::Status::Error(ErrorCode::notready, "test unavailable"));
    auto first_result = co_await std::move(first).wrap();
    auto joined_result = co_await std::move(joined).wrap();
    ASSERT_TRUE(first_result.is_error() && joined_result.is_error());
    ASSERT_EQ(first_result.error().code(), ErrorCode::notready);
    ASSERT_EQ(joined_result.error().message(), first_result.error().message());
    auto stats = co_await td::actor::ask(pool.get(), &SharedAdmissionTestPool::diagnostics);
    ASSERT_TRUE(stats.find("shard_shared_active:0") != std::string::npos);
    ASSERT_TRUE(stats.find("shard_shared_waiters:0") != std::string::npos);
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, SharedAdmissionValidStateIsProjectedAndCachedOnce) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto requests = std::make_shared<SharedAdmissionManagerRequests>();
    auto manager = td::actor::create_actor<SharedAdmissionTestManager>("shared-admission-manager", requests);
    auto pool = td::actor::create_actor<SharedAdmissionTestPool>("shared-admission-pool", manager.get(), true);
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    co_await td::actor::ask(pool.get(), &SharedAdmissionTestPool::initialize_cache, mc);
    auto first = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(5));
    auto joined = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(8));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->requests.size(), 1u);
    requests->requests[0].promise.set_value(td::make_ref<SharedAdmissionTestState>(shard));
    auto first_view = co_await std::move(first);
    auto joined_view = co_await std::move(joined);
    ASSERT_TRUE(first_view == joined_view);
    ASSERT_EQ(first_view->block_id, shard);
    ASSERT_EQ(first_view->gen_utime, 100u);
    auto stats = co_await td::actor::ask(pool.get(), &SharedAdmissionTestPool::cache_diagnostics);
    ASSERT_TRUE(stats.find("shard_manager_waits:1") != std::string::npos);
    ASSERT_TRUE(stats.find("shard_cache_fills:1") != std::string::npos);
    ASSERT_TRUE(stats.find("shard_cache_fill_races:0") != std::string::npos);
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, SharedAdmissionSuccessNeverOutlivesShortCallerDeadline) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto requests = std::make_shared<SharedAdmissionManagerRequests>();
    auto manager = td::actor::create_actor<SharedAdmissionTestManager>("shared-admission-manager", requests);
    auto pool = td::actor::create_actor<SharedAdmissionTestPool>("shared-admission-pool", manager.get(), true);
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    auto first = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(5));
    auto joined = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(1));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->requests.size(), 1u);
    requests->requests[0].promise.set_value(td::make_ref<SharedAdmissionTestState>(shard));
    // The success is queued, but its delivery and the timeout now race on the
    // pool actor. Only the first caller still owns usable admission time.
    scheduler.advance_time(2);
    co_await scheduler.wait_sync_work();
    ASSERT_TRUE((co_await std::move(first).wrap()).is_ok());
    ASSERT_TRUE((co_await std::move(joined).wrap()).is_error());
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, DisabledSharedAdmissionRetainsIndependentManagerRequests) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto requests = std::make_shared<SharedAdmissionManagerRequests>();
    auto manager = td::actor::create_actor<SharedAdmissionTestManager>("shared-admission-manager", requests);
    auto pool = td::actor::create_actor<SharedAdmissionTestPool>("shared-admission-pool", manager.get(), false);
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    auto first = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(5));
    auto second = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(8));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->requests.size(), 2u);
    for (auto &request : requests->requests) {
      request.promise.set_error(td::Status::Error(ErrorCode::notready, "test unavailable"));
    }
    ASSERT_TRUE((co_await std::move(first).wrap()).is_error());
    ASSERT_TRUE((co_await std::move(second).wrap()).is_error());
    auto stats = co_await td::actor::ask(pool.get(), &SharedAdmissionTestPool::diagnostics);
    ASSERT_TRUE(stats.find("shard_sharing_enabled:0") != std::string::npos);
    ASSERT_TRUE(stats.find("shard_shared_dispatches:0") != std::string::npos);
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, SharedAdmissionEarlyManagerTimeoutDoesNotStartExtraRequests) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto requests = std::make_shared<SharedAdmissionManagerRequests>();
    auto manager = td::actor::create_actor<SharedAdmissionTestManager>("shared-admission-manager", requests);
    auto pool = td::actor::create_actor<SharedAdmissionTestPool>("shared-admission-pool", manager.get(), true);
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    auto first = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(5));
    auto joined = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(8));
    co_await scheduler.wait_sync_work();
    requests->requests[0].promise.set_error(td::Status::Error(ErrorCode::timeout, "early manager timeout"));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->requests.size(), 1u);
    ASSERT_TRUE(first.await_ready() && joined.await_ready());
    ASSERT_TRUE((co_await std::move(first).wrap()).is_error());
    ASSERT_TRUE((co_await std::move(joined).wrap()).is_error());
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, SharedAdmissionLaterCallerRetriesOnceWithinOriginalDeadline) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto requests = std::make_shared<SharedAdmissionManagerRequests>();
    auto manager = td::actor::create_actor<SharedAdmissionTestManager>("shared-admission-manager", requests);
    auto pool = td::actor::create_actor<SharedAdmissionTestPool>("shared-admission-pool", manager.get(), true);
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    auto short_deadline = td::Timestamp::in(1);
    auto later_deadline = td::Timestamp::in(8);
    auto first = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, short_deadline);
    auto joined = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, later_deadline);
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->requests.size(), 1u);
    ASSERT_EQ(requests->requests[0].deadline.get(), short_deadline.get());
    scheduler.advance_time(2);
    co_await scheduler.wait_sync_work();
    ASSERT_TRUE(first.await_ready());
    auto first_result = co_await std::move(first).wrap();
    ASSERT_TRUE(first_result.is_error());
    ASSERT_TRUE(!joined.await_ready());
    ASSERT_EQ(requests->requests.size(), 2u);
    ASSERT_EQ(requests->requests[1].deadline.get(), later_deadline.get());
    // A late response to the abandoned first manager wait cannot complete or
    // erase the second caller's new request.
    requests->requests[0].promise.set_error(td::Status::Error(ErrorCode::notready, "late first response"));
    co_await scheduler.wait_sync_work();
    ASSERT_TRUE(!joined.await_ready());
    requests->requests[1].promise.set_error(td::Status::Error(ErrorCode::notready, "fallback unavailable"));
    auto joined_result = co_await std::move(joined).wrap();
    ASSERT_TRUE(joined_result.is_error());
    ASSERT_EQ(joined_result.error().message(), "fallback unavailable");
    ASSERT_EQ(requests->requests.size(), 2u);
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, SharedAdmissionActorShutdownCancelsPendingFetchSafely) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto requests = std::make_shared<SharedAdmissionManagerRequests>();
    auto manager = td::actor::create_actor<SharedAdmissionTestManager>("shared-admission-manager", requests);
    auto destroyed = std::make_shared<bool>(false);
    auto pool = td::actor::create_actor<SharedAdmissionTestPool>("shared-admission-pool", manager.get(), true, destroyed);
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    auto first = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(5));
    auto joined = td::actor::ask(pool.get(), &SharedAdmissionTestPool::get, mc, shard, td::Timestamp::in(8));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(requests->requests.size(), 1u);
    td::actor::send_closure(pool.get(), &SharedAdmissionTestPool::shutdown);
    co_await scheduler.wait_sync_work();
    requests->requests[0].promise.set_error(td::Status::Error(ErrorCode::notready, "after shutdown"));
    co_await scheduler.wait_sync_work();
    // Stopped actors cannot run the fanout continuation. The independent
    // original caller timers must still release both requests safely.
    scheduler.advance_time(10);
    co_await scheduler.wait_sync_work();
    ASSERT_TRUE(first.await_ready() && joined.await_ready());
    ASSERT_TRUE((co_await std::move(first).wrap()).is_error());
    ASSERT_TRUE((co_await std::move(joined).wrap()).is_error());
    pool.reset();
    co_await scheduler.wait_sync_work();
    ASSERT_TRUE(*destroyed);
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, SharedAdmissionViewsRequireExactMasterchainAndShardIdentity) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto pool = ExtMessagePoolTestAccess::make_pool();
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto mc_fork = ExtMessagePoolTestAccess::masterchain_state(42, 1)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    auto shard_fork = ExtMessagePoolTestAccess::shard_top(9, 1);
    auto deadline = td::Timestamp::in(5);
    auto first = ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard, deadline);
    auto joined = ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard, deadline);
    auto other_mc = ExtMessagePoolTestAccess::queue_admission_view(pool, mc_fork, shard, deadline);
    auto other_shard = ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard_fork, deadline);
    ASSERT_TRUE(first && first.value().dispatch);
    ASSERT_TRUE(joined && !joined.value().dispatch);
    ASSERT_TRUE(other_mc && other_mc.value().dispatch);
    ASSERT_TRUE(other_shard && other_shard.value().dispatch);
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_entries(pool), 3u);
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_waiters(pool), 4u);
    auto view = ExtMessagePoolTestAccess::admission_view(shard, 77);
    ExtMessagePoolTestAccess::complete_shared_admission_view(pool, mc, shard, view);
    auto first_view = co_await std::move(first.value().waiter);
    auto joined_view = co_await std::move(joined.value().waiter);
    ASSERT_TRUE(first_view == view && joined_view == view);
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_entries(pool), 2u);
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_waiters(pool), 2u);
    ExtMessagePoolTestAccess::complete_shared_admission_view(pool, mc_fork, shard,
                                                            td::Status::Error(ErrorCode::notready, "old fork"));
    ExtMessagePoolTestAccess::complete_shared_admission_view(pool, mc, shard_fork,
                                                            td::Status::Error("invalid root"));
    ASSERT_TRUE((co_await std::move(other_mc.value().waiter).wrap()).is_error());
    ASSERT_TRUE((co_await std::move(other_shard.value().waiter).wrap()).is_error());
    auto stats = ExtMessagePoolTestAccess::shared_admission_stats(pool);
    ASSERT_TRUE(stats.find("shard_shared_dispatches:3") != std::string::npos);
    ASSERT_TRUE(stats.find("shard_shared_joins:1") != std::string::npos);
    ASSERT_TRUE(stats.find("shard_shared_completions:3") != std::string::npos);
    ASSERT_TRUE(stats.find("shard_shared_errors:2") != std::string::npos);
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_entries(pool), 0u);
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_waiters(pool), 0u);
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, SharedAdmissionCallerTimeoutDoesNotCancelOtherWaiters) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto pool = ExtMessagePoolTestAccess::make_pool();
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    auto first_deadline = td::Timestamp::in(10);
    auto first = ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard, first_deadline);
    auto short_deadline = td::Timestamp::in(1);
    auto joined = ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard, short_deadline);
    auto short_wait = td::actor::await_with_timeout(std::move(joined.value().waiter), short_deadline).start();
    co_await scheduler.wait_sync_work();
    scheduler.advance_time(2);
    co_await scheduler.wait_sync_work();
    ASSERT_TRUE(short_wait.await_ready());
    auto timed_out = co_await std::move(short_wait).wrap();
    ASSERT_TRUE(timed_out.is_error());
    ASSERT_EQ(timed_out.error().code(), td::actor::AWAIT_TIMEOUT_CODE);
    ASSERT_TRUE(!first.value().waiter.await_ready());
    auto view = ExtMessagePoolTestAccess::admission_view(shard, 77);
    ExtMessagePoolTestAccess::complete_shared_admission_view(pool, mc, shard, view);
    ASSERT_TRUE((co_await std::move(first.value().waiter)) == view);
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_waiters(pool), 0u);
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, SharedAdmissionCreatorAbandonmentDoesNotCancelJoinedWaiter) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto pool = ExtMessagePoolTestAccess::make_pool();
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    auto first = ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard, td::Timestamp::in(5));
    auto joined = ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard, td::Timestamp::in(8));
    first.value().waiter.detach_silent();
    auto view = ExtMessagePoolTestAccess::admission_view(shard, 77);
    ExtMessagePoolTestAccess::complete_shared_admission_view(pool, mc, shard, view);
    ASSERT_TRUE((co_await std::move(joined.value().waiter)) == view);
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_entries(pool), 0u);
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, SharedAdmissionOldGenerationCompletesWithoutPopulatingNewCache) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto pool = ExtMessagePoolTestAccess::make_pool();
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto next_mc = ExtMessagePoolTestAccess::masterchain_state(43)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    ExtMessagePoolTestAccess::reset_admission_generation(pool, mc);
    auto first = ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard, td::Timestamp::in(5));
    ExtMessagePoolTestAccess::reset_admission_generation(pool, next_mc);
    auto next = ExtMessagePoolTestAccess::queue_admission_view(pool, next_mc, shard, td::Timestamp::in(5));
    ASSERT_TRUE(next.value().dispatch);
    auto old_view = ExtMessagePoolTestAccess::admission_view(shard, 77);
    auto stored = ExtMessagePoolTestAccess::store_admission_view(pool, mc, old_view);
    ASSERT_TRUE(stored.is_ok());
    ExtMessagePoolTestAccess::complete_shared_admission_view(pool, mc, shard, stored.move_as_ok());
    ASSERT_TRUE((co_await std::move(first.value().waiter)) == old_view);
    ASSERT_TRUE(!ExtMessagePoolTestAccess::lookup_admission_view(pool, next_mc, shard));
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_entries(pool), 1u);
    ExtMessagePoolTestAccess::complete_shared_admission_view(pool, next_mc, shard, old_view);
    ASSERT_TRUE((co_await std::move(next.value().waiter)) == old_view);
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, SharedAdmissionLimitsFallBackWithoutEvictingLiveWaiters) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto pool = ExtMessagePoolTestAccess::make_pool();
    auto mc = ExtMessagePoolTestAccess::masterchain_state(42)->get_block_id();
    auto shard = ExtMessagePoolTestAccess::shard_top(9);
    auto deadline = td::Timestamp::in(5);
    for (unsigned i = 0; i < 256; ++i) {
      auto request = ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard, deadline);
      ASSERT_TRUE(request);
      ASSERT_EQ(request.value().dispatch, i == 0);
    }
    ASSERT_TRUE(!ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard, deadline));
    for (unsigned i = 1; i < 64; ++i) {
      ASSERT_TRUE(ExtMessagePoolTestAccess::queue_admission_view(
          pool, mc, ExtMessagePoolTestAccess::shard_top(9 + i), deadline));
    }
    ASSERT_TRUE(!ExtMessagePoolTestAccess::queue_admission_view(
        pool, mc, ExtMessagePoolTestAccess::shard_top(100), deadline));
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_entries(pool), 64u);
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_waiters(pool), 319u);
    auto stats = ExtMessagePoolTestAccess::shared_admission_stats(pool);
    ASSERT_TRUE(stats.find("shard_shared_table_full:1") != std::string::npos);
    ASSERT_TRUE(stats.find("shard_shared_waiters_full:1") != std::string::npos);
    // Expired work is not joined or replaced; its completion still owns its
    // entry and each later request falls back under its own deadline.
    scheduler.advance_time(6);
    ASSERT_TRUE(!ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard, td::Timestamp::in(5)));
    for (unsigned i = 0; i < 64; ++i) {
      ExtMessagePoolTestAccess::complete_shared_admission_view(
          pool, mc, ExtMessagePoolTestAccess::shard_top(9 + i), td::Status::Error(ErrorCode::timeout, "deadline"));
    }
    auto retry = ExtMessagePoolTestAccess::queue_admission_view(pool, mc, shard, td::Timestamp::in(5));
    ASSERT_TRUE(retry && retry.value().dispatch);
    ExtMessagePoolTestAccess::complete_shared_admission_view(pool, mc, shard,
                                                            td::Status::Error(ErrorCode::notready, "retry"));
    ASSERT_TRUE((co_await std::move(retry.value().waiter).wrap()).is_error());
    ASSERT_EQ(ExtMessagePoolTestAccess::shared_admission_waiters(pool), 0u);
    co_return {};
  });
}

TEST(ExtMessagePoolScheduler, NativeAdmissionPinsFreshLocallyAppliedMasterchainState) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto state_100 = ExtMessagePoolTestAccess::masterchain_state(100);
  pool.update_last_masterchain_state(state_100);
  auto pinned_100 = ExtMessagePoolTestAccess::pin_native_admission_state(pool);

  auto state_102 = ExtMessagePoolTestAccess::masterchain_state(102);
  pool.update_last_masterchain_state(state_102);
  ASSERT_EQ(ExtMessagePoolTestAccess::pin_native_admission_state(pool)->get_block_id(), state_102->get_block_id());

  // A delayed notification and a conflicting same-height revision cannot move
  // the admission view away from the newest locally applied canonical state.
  pool.update_last_masterchain_state(ExtMessagePoolTestAccess::masterchain_state(101));
  pool.update_last_masterchain_state(ExtMessagePoolTestAccess::masterchain_state(102, 1));
  ASSERT_EQ(ExtMessagePoolTestAccess::pin_native_admission_state(pool)->get_block_id(), state_102->get_block_id());
  ASSERT_EQ(ExtMessagePoolTestAccess::ignored_masterchain_updates(pool), 2u);

  // The coroutine-local Ref remains pinned even after the actor receives a new
  // applied state, so every shard lookup in one batch uses one MC revision.
  ASSERT_EQ(pinned_100->get_block_id(), state_100->get_block_id());
}

TEST(ExtMessagePoolScheduler, LocalAcceptDoesNotAdvanceWatermarkBeforeCanonicalAccountState) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(83);
  ASSERT_TRUE(ExtMessagePoolTestAccess::observe_account_state(pool, source, 7, 1'000, 100));
  auto initial_revision = ExtMessagePoolTestAccess::watermark_revision(pool, source);
  auto hash = ExtMessagePoolTestAccess::add(pool, source, 7);

  // A successful local accept may still be replaced across a catchain
  // rotation. Tracking it is reversible and must leave both the message and
  // the canonical nonce watermark untouched.
  ExtMessagePoolTestAccess::track_locally_accepted(pool, {{source, 7, hash}});
  ASSERT_EQ(ExtMessagePoolTestAccess::watermark_revision(pool, source), initial_revision);
  ASSERT_EQ(ExtMessagePoolTestAccess::first_unconsumed_nonce(pool, source), td::optional<td::uint64>(7));
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, hash));
  ASSERT_EQ(ExtMessagePoolTestAccess::tracked_nonce(pool, source), td::optional<td::uint64>(7));

  // Only the exact shard state referenced by an applied MC state authorizes
  // the prefix purge. Its next nonce is exclusive: purge nonce < 8.
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 8, 990, 102, 102));
  ASSERT_TRUE(ExtMessagePoolTestAccess::watermark_revision(pool, source) > initial_revision);
  ASSERT_EQ(ExtMessagePoolTestAccess::first_unconsumed_nonce(pool, source), td::optional<td::uint64>(8));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::tracked_nonce(pool, source));
}

TEST(ExtMessagePoolScheduler, NativeReconciliationTracksEitherRunOrChildMetadata) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(97);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 10);
  auto hash = ExtMessagePoolTestAccess::add_work(pool, source, 10, 3);

  // Current v5 metadata describes one atomic parent interval. Retain coverage
  // for legacy/per-child metadata too: both forms must register the same
  // exclusive reconciliation target.
  ExtMessagePoolTestAccess::track_locally_accepted_records(
      pool, {TrackedNativeExternalMessage{
                .hash = hash, .workchain = source.first, .source = source.second, .nonce = 10, .logical_count = 3}});
  ASSERT_EQ(ExtMessagePoolTestAccess::tracked_nonce(pool, source), td::optional<td::uint64>(12));
  ASSERT_EQ(ExtMessagePoolTestAccess::reconciliation_tracked_logical_messages(pool), 3u);

  auto child_pool = ExtMessagePoolTestAccess::make_pool();
  ExtMessagePoolTestAccess::set_watermark(child_pool, source, 10);
  auto child_hash = ExtMessagePoolTestAccess::add_work(child_pool, source, 10, 3);
  ExtMessagePoolTestAccess::track_locally_accepted_records(
      child_pool, {TrackedNativeExternalMessage{
                       .hash = child_hash, .workchain = source.first, .source = source.second, .nonce = 10},
                   TrackedNativeExternalMessage{
                       .hash = child_hash, .workchain = source.first, .source = source.second, .nonce = 11},
                   TrackedNativeExternalMessage{
                       .hash = child_hash, .workchain = source.first, .source = source.second, .nonce = 12}});
  ASSERT_EQ(ExtMessagePoolTestAccess::tracked_nonce(child_pool, source), td::optional<td::uint64>(12));
  ASSERT_EQ(ExtMessagePoolTestAccess::reconciliation_tracked_logical_messages(child_pool), 3u);
}

TEST(ExtMessagePoolScheduler, CancelledDuplicateSeqnoIsNotAnAppliedOutcome) {
  using consensus::AcceptBlockAttemptDecision;
  ASSERT_EQ(consensus::classify_accept_block_attempt(false, ErrorCode::cancelled),
            AcceptBlockAttemptDecision::cancelled);
  ASSERT_EQ(consensus::classify_accept_block_attempt(false, ErrorCode::timeout), AcceptBlockAttemptDecision::retry);
  ASSERT_EQ(consensus::classify_accept_block_attempt(false, ErrorCode::notready), AcceptBlockAttemptDecision::retry);
  ASSERT_EQ(consensus::classify_accept_block_attempt(true, 0), AcceptBlockAttemptDecision::applied);
}

TEST(ExtMessagePoolScheduler, OldSessionAcceptThenReplacementWaitsForCanonicalAccountState) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto losing_source_a = ExtMessagePoolTestAccess::source(80);
  auto losing_source_b = ExtMessagePoolTestAccess::source(81);
  auto winning_source = ExtMessagePoolTestAccess::source(82);
  for (auto source : {losing_source_a, losing_source_b, winning_source}) {
    ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  }
  auto losing_hash_a = ExtMessagePoolTestAccess::add(pool, losing_source_a, 0);
  auto losing_hash_b = ExtMessagePoolTestAccess::add(pool, losing_source_b, 0);
  auto winning_hash = ExtMessagePoolTestAccess::add(pool, winning_source, 0);

  // The old session can successfully accept a root before a replacement wins
  // the same seqno in the new session. Both accepts are tracked, but neither
  // is allowed to mutate the pool.
  ExtMessagePoolTestAccess::track_locally_accepted(
      pool, {{losing_source_a, 0, losing_hash_a}, {losing_source_b, 0, losing_hash_b}});
  ExtMessagePoolTestAccess::track_locally_accepted(pool, {{winning_source, 0, winning_hash}});
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, winning_hash));
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, losing_hash_a));
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, losing_hash_b));

  // The applied MC-referenced winner advances only its canonical source. The
  // old-session-only messages remain eligible because their account nonces did
  // not advance in the winning state.
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, winning_source, 1));
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, losing_source_a, 0));
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, losing_source_b, 0));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, winning_hash));
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, losing_hash_a));
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, losing_hash_b));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::tracked_nonce(pool, winning_source));
  ASSERT_EQ(ExtMessagePoolTestAccess::tracked_nonce(pool, losing_source_a), td::optional<td::uint64>(0));
  ASSERT_EQ(ExtMessagePoolTestAccess::tracked_nonce(pool, losing_source_b), td::optional<td::uint64>(0));

  auto still_dispatchable = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 8);
  ASSERT_EQ(still_dispatchable.sources.size(), 2u);
  ASSERT_TRUE(std::find(still_dispatchable.sources.begin(), still_dispatchable.sources.end(), losing_source_a) !=
              still_dispatchable.sources.end());
  ASSERT_TRUE(std::find(still_dispatchable.sources.begin(), still_dispatchable.sources.end(), losing_source_b) !=
              still_dispatchable.sources.end());
}

TEST(ExtMessagePoolScheduler, AppliedCanonicalStateCoversPendingSourceWithoutLocalAccept) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(84);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 4);
  auto hash = ExtMessagePoolTestAccess::add(pool, source, 4);

  ASSERT_TRUE(!ExtMessagePoolTestAccess::tracked_nonce(pool, source));
  // Model the target registration performed for every shard-client-applied
  // masterchain state. No local consensus callback is required.
  ExtMessagePoolTestAccess::register_pending_reconciliation_targets(pool);
  ASSERT_EQ(ExtMessagePoolTestAccess::tracked_nonce(pool, source), td::optional<td::uint64>(4));

  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 5, 990, 102, 102));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::tracked_nonce(pool, source));
}

TEST(ExtMessagePoolScheduler, AppliedStateBeforeLocalTrackDoesNotRescanUnchangedShardTop) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(86);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto hash = ExtMessagePoolTestAccess::add(pool, source, 0);
  auto shard_top = ExtMessagePoolTestAccess::shard_top(100);

  // Model a complete applied-state scan followed by the local BlockAccepter
  // notification for a candidate built after that immutable state. Tracking
  // the new hint must not force thousands of sources through the old state a
  // second time; only a different canonical shard top can contain it.
  ASSERT_TRUE(ExtMessagePoolTestAccess::should_reconcile_shard_top(pool, shard_top, 1));
  ExtMessagePoolTestAccess::record_successful_shard_reconciliation(pool, shard_top);
  ExtMessagePoolTestAccess::track_locally_accepted(pool, {{source, 0, hash}});
  ASSERT_TRUE(!ExtMessagePoolTestAccess::should_reconcile_shard_top(pool, shard_top, 1));
  ASSERT_EQ(ExtMessagePoolTestAccess::unchanged_top_skips(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::unchanged_source_skips(pool), 1u);
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, hash));
  ASSERT_EQ(ExtMessagePoolTestAccess::tracked_nonce(pool, source), td::optional<td::uint64>(0));

  ASSERT_TRUE(ExtMessagePoolTestAccess::should_reconcile_shard_top(
      pool, ExtMessagePoolTestAccess::shard_top(101), 1));
}

TEST(ExtMessagePoolScheduler, FailedShardReconciliationRetriesSameTop) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto shard_top = ExtMessagePoolTestAccess::shard_top(200);

  ASSERT_TRUE(ExtMessagePoolTestAccess::should_reconcile_shard_top(pool, shard_top, 3));
  // An incomplete fetch/account walk deliberately records no success.
  ASSERT_TRUE(ExtMessagePoolTestAccess::should_reconcile_shard_top(pool, shard_top, 3));
  ASSERT_EQ(ExtMessagePoolTestAccess::unchanged_top_skips(pool), 0u);
  ASSERT_EQ(ExtMessagePoolTestAccess::unchanged_source_skips(pool), 0u);

  ExtMessagePoolTestAccess::record_successful_shard_reconciliation(pool, shard_top);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::should_reconcile_shard_top(pool, shard_top, 3));
  ASSERT_EQ(ExtMessagePoolTestAccess::unchanged_top_skips(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::unchanged_source_skips(pool), 3u);
}

TEST(ExtMessagePoolScheduler, ExactRetryPreservesCommittedEntryAcrossCanonicalRevisionAdvance) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(87);
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 7, 100, 100, 100));
  auto original_revision = ExtMessagePoolTestAccess::watermark_revision(pool, source);
  auto hash = ExtMessagePoolTestAccess::add(pool, source, 7, 0, true, true, 10);

  // Model the narrow ordering window in which a fresh canonical snapshot has
  // advanced the watermark but the surviving reservation has not yet been
  // rebased. A byte-identical retry is idempotent and must never erase the
  // committed head merely because its revision is older.
  ASSERT_TRUE(ExtMessagePoolTestAccess::observe_account_state(pool, source, 7, 90, 101));
  ASSERT_TRUE(ExtMessagePoolTestAccess::watermark_revision(pool, source) > original_revision);
  ASSERT_EQ(ExtMessagePoolTestAccess::reservation_revision(pool, source, 7), original_revision);
  ASSERT_TRUE(ExtMessagePoolTestAccess::exact_retry_is_idempotent(pool, hash));
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, hash));
  ASSERT_EQ(ExtMessagePoolTestAccess::exact_retry_preserved_stale_revision(pool), 1u);

  // Applying that canonical snapshot rebases the affordable survivor. The
  // source remains immediately dispatchable without a repair retry.
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 7, 90, 101, 101));
  ASSERT_EQ(ExtMessagePoolTestAccess::reservation_revision(pool, source, 7),
            ExtMessagePoolTestAccess::watermark_revision(pool, source));
  ASSERT_EQ(ExtMessagePoolTestAccess::rebased_reservations(pool), 1u);
  auto ready = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 8);
  ASSERT_EQ(ready.nonces, (std::vector<td::uint64>{7}));
}

TEST(ExtMessagePoolScheduler, ExactRetryRequiresMatchingCommittedReservationHash) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(89);
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 3, 100, 100, 100));
  auto hash = ExtMessagePoolTestAccess::add(pool, source, 3, 0, true, true, 10);
  ExtMessagePoolTestAccess::set_reservation_hash(pool, source, 3, make_bits(999, 189));
  ASSERT_TRUE(ExtMessagePoolTestAccess::observe_account_state(pool, source, 3, 90, 101));

  // Nonce equality is insufficient for idempotence: an inconsistent hash must
  // not preserve the old pool object as the current committed reservation.
  ASSERT_TRUE(!ExtMessagePoolTestAccess::exact_retry_is_idempotent(pool, hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, hash));
  ASSERT_EQ(ExtMessagePoolTestAccess::reservation_nonces(pool, source), (std::vector<td::uint64>{3}));
  ASSERT_EQ(ExtMessagePoolTestAccess::exact_retry_preserved_stale_revision(pool), 0u);
}

TEST(ExtMessagePoolScheduler, ReconciliationTelemetrySeparatesCanonicalFactsFromAdmissionProgress) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  const auto source = ExtMessagePoolTestAccess::source(91);
  auto apply = [&](td::uint64 nonce, td::uint64 balance, LogicalTime lt) {
    return ExtMessagePoolTestAccess::reconcile_account_with_telemetry(pool, source, nonce, balance, 100, lt);
  };
  ASSERT_TRUE(apply(0, 100, 10).ok());
  ASSERT_TRUE(!apply(0, 100, 20).ok());
  ASSERT_TRUE(apply(1, 90, 30).ok());
  ASSERT_TRUE(apply(1, 110, 40).ok());
  const auto revision = ExtMessagePoolTestAccess::watermark_revision(pool, source);
  ASSERT_TRUE(apply(1, 0, 35).is_error());
  ASSERT_EQ(ExtMessagePoolTestAccess::watermark_revision(pool, source), revision);
  // A lower input nonce never moves the watermark backwards. With unchanged
  // balance the effective observed account facts remain unchanged.
  ASSERT_TRUE(!apply(0, 110, 50).ok());
  ASSERT_EQ(ExtMessagePoolTestAccess::first_unconsumed_nonce(pool, source).value(), 1u);

  const auto &t = ExtMessagePoolTestAccess::reconciliation_telemetry(pool);
  ASSERT_EQ(t.apply_calls, 6u);
  ASSERT_EQ(t.apply_first_observation, 1u);
  ASSERT_EQ(t.apply_nonce_advanced, 1u);
  ASSERT_EQ(t.apply_balance_only_changed, 1u);
  ASSERT_EQ(t.apply_unchanged, 2u);
  ASSERT_EQ(t.apply_errors, 1u);
  ASSERT_EQ(t.apply_stale_lt, 1u);
  ASSERT_EQ(t.apply_balance_increased, 1u);
  ASSERT_EQ(t.apply_balance_decreased, 1u);
  ASSERT_EQ(t.apply_effects, 3u);
  ASSERT_EQ(t.apply.samples, 0u);

  // This is the same apply entry point called by admission. Its historical
  // progress increments must not contaminate reconciliation-only denominators.
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 2, 100, 100, 60));
  ASSERT_EQ(ExtMessagePoolTestAccess::historical_sources_advanced(pool), 2u);
  ASSERT_EQ(t.apply_calls, 6u);
  ASSERT_EQ(t.apply_nonce_advanced, 1u);
}

TEST(ExtMessagePoolScheduler, ReconciliationTelemetryCountsActualPrefixWorkAndPruning) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  const auto source = ExtMessagePoolTestAccess::source(92);
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 0, 100, 100, 100));
  auto keep = ExtMessagePoolTestAccess::add(pool, source, 0, 0, true, true, 30);
  auto unaffordable = ExtMessagePoolTestAccess::add(pool, source, 1, 0, true, true, 20);
  auto tail = ExtMessagePoolTestAccess::add(pool, source, 2, 0, true, true, 1);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::reconcile_account_with_telemetry(pool, source, 0, 100, 101, 101).ok());
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account_with_telemetry(pool, source, 0, 40, 102, 102).ok());
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, keep));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, unaffordable));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, tail));
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account_with_telemetry(pool, source, 1, 10, 103, 103).ok());
  ASSERT_TRUE(ExtMessagePoolTestAccess::reservation_nonces(pool, source).empty());
  const auto &t = ExtMessagePoolTestAccess::reconciliation_telemetry(pool);
  ASSERT_EQ(t.apply_calls, 3u);
  ASSERT_EQ(t.apply_unchanged, 1u);
  ASSERT_EQ(t.apply_balance_only_changed, 1u);
  ASSERT_EQ(t.apply_nonce_advanced, 1u);
  ASSERT_EQ(t.pending_reservations_before_apply_sum, 7u);
  ASSERT_EQ(t.reservation_prefix_entries, 6u);
  ASSERT_EQ(t.messages_purged, 1u);
  ASSERT_EQ(t.reservation_rebases, 1u);
  ASSERT_EQ(t.tail_prunes, 2u);
  ASSERT_EQ(t.apply_effects, 2u);
}

TEST(ExtMessagePoolScheduler, ReconciliationUnchangedAccountCanStillExpirePendingWork) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  const auto source = ExtMessagePoolTestAccess::source(93);
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 0, 100, 100, 100));
  auto hash = ExtMessagePoolTestAccess::add(pool, source, 0);
  ExtMessagePoolTestAccess::set_valid_until(pool, source, 0, 101);
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account_with_telemetry(pool, source, 0, 100, 102, 102).ok());
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, hash));
  const auto &t = ExtMessagePoolTestAccess::reconciliation_telemetry(pool);
  ASSERT_EQ(t.apply_unchanged, 1u);
  ASSERT_EQ(t.apply_effects, 1u);
  ASSERT_EQ(t.pending_reservations_before_apply_sum, 1u);
  ASSERT_EQ(t.reservation_prefix_entries, 0u);
  ASSERT_EQ(t.messages_purged, 1u);
}

TEST(ExtMessagePoolScheduler, ReconciliationRegistrationAttributionAndTimingAreOptIn) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  const auto source = ExtMessagePoolTestAccess::source(94);
  ExtMessagePoolTestAccess::add(pool, source, 0);
  ExtMessagePoolTestAccess::add(pool, ExtMessagePoolTestAccess::source(95), 0);
  ExtMessagePoolTestAccess::register_pending_reconciliation_targets(pool);
  ExtMessagePoolTestAccess::register_pending_reconciliation_targets(pool);
  const auto &t = ExtMessagePoolTestAccess::reconciliation_telemetry(pool);
  ASSERT_EQ(t.register_source_visits, 4u);
  ASSERT_EQ(t.register_tracked_visits, 2u);
  ASSERT_EQ(t.registration.samples, 0u);

  ExtMessagePoolTestAccess::set_reconciliation_profile(pool, true);
  ExtMessagePoolTestAccess::register_pending_reconciliation_targets(pool);
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account_with_telemetry(pool, source, 0, 100, 100, 100).ok());
  ASSERT_EQ(t.register_source_visits, 6u);
  ASSERT_EQ(t.register_tracked_visits, 4u);
  ASSERT_EQ(t.registration.samples, 1u);
  ASSERT_EQ(t.apply.samples, 1u);
  const auto stats = ExtMessagePoolTestAccess::reconciliation_diagnostics(pool);
  ASSERT_TRUE(stats.find(" profile_enabled:1") != std::string::npos);
  ASSERT_TRUE(stats.find(" register_source_visits:6") != std::string::npos);
  ASSERT_TRUE(stats.find(" apply_samples:1") != std::string::npos);
}

TEST(ExtMessagePoolScheduler, CanonicalBalancePrunesFirstUnaffordableReservationAndTail) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(88);
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 0, 100, 100, 100));
  auto keep = ExtMessagePoolTestAccess::add(pool, source, 0, 0, true, true, 30);
  auto first_unaffordable = ExtMessagePoolTestAccess::add(pool, source, 1, 0, true, true, 20);
  auto tail = ExtMessagePoolTestAccess::add(pool, source, 2, 0, true, true, 1);

  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 0, 40, 101, 101));
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, keep));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, first_unaffordable));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, tail));
  ASSERT_EQ(ExtMessagePoolTestAccess::reservation_nonces(pool, source), (std::vector<td::uint64>{0}));
  ASSERT_EQ(ExtMessagePoolTestAccess::reservation_revision(pool, source, 0),
            ExtMessagePoolTestAccess::watermark_revision(pool, source));
  ASSERT_EQ(ExtMessagePoolTestAccess::rebased_reservations(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::unaffordable_tail_pruned(pool), 2u);

  auto ready = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 8);
  ASSERT_EQ(ready.nonces, (std::vector<td::uint64>{0}));
  ASSERT_EQ(ready.head_gaps, 0u);
}

TEST(ExtMessagePoolScheduler, CanonicalAdvancePrunesStaleUncommittedHeadAndTail) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(90);
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 0, 100, 100, 100));
  auto uncommitted_head = ExtMessagePoolTestAccess::add(pool, source, 0, 0, true, false, 10);
  auto committed_suffix = ExtMessagePoolTestAccess::add(pool, source, 1, 0, true, true, 10);

  // A reservation whose verification predates the canonical revision is never
  // silently blessed. Removing its entire suffix avoids replacing a stale
  // uncommitted head with a permanent nonce hole.
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 0, 90, 101, 101));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, uncommitted_head));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, committed_suffix));
  ASSERT_TRUE(ExtMessagePoolTestAccess::reservation_nonces(pool, source).empty());
  ASSERT_EQ(ExtMessagePoolTestAccess::rebased_reservations(pool), 0u);
  ASSERT_EQ(ExtMessagePoolTestAccess::stale_uncommitted_tail_pruned(pool), 2u);
}

TEST(ExtMessagePoolScheduler, WholeStateFingerprintSkipsOnlyAfterSuccessfulPublication) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto top = ExtMessagePoolTestAccess::shard_top(300);
  auto replacement = ExtMessagePoolTestAccess::shard_top(300, 1);

  // Merely seeing a fingerprint is not enough: an incomplete state/account
  // walk must retry. Only explicit successful publication enables the early
  // all-source gate.
  ASSERT_TRUE(!ExtMessagePoolTestAccess::should_skip_state_fingerprint(pool, {top}));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::should_skip_state_fingerprint(pool, {top}));
  ExtMessagePoolTestAccess::record_successful_state_fingerprint(pool, {top});
  ASSERT_TRUE(ExtMessagePoolTestAccess::should_skip_state_fingerprint(pool, {top}));
  ASSERT_EQ(ExtMessagePoolTestAccess::unchanged_state_skips(pool), 1u);

  // Block identity includes both hashes, so a same-height replacement is a
  // different fingerprint and cannot inherit the successful gate.
  ASSERT_TRUE(!ExtMessagePoolTestAccess::should_skip_state_fingerprint(pool, {replacement}));
  ASSERT_EQ(ExtMessagePoolTestAccess::unchanged_state_skips(pool), 1u);
}

TEST(ExtMessagePoolScheduler, ExpiredLastReservationPrunesReconciliationTarget) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(85);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto hash = ExtMessagePoolTestAccess::add(pool, source, 0);
  ExtMessagePoolTestAccess::register_pending_reconciliation_targets(pool);
  ASSERT_EQ(ExtMessagePoolTestAccess::tracked_nonce(pool, source), td::optional<td::uint64>(0));

  ExtMessagePoolTestAccess::expire(pool, hash);
  pool.cleanup_external_messages({basechainId, shardIdAll});
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::tracked_nonce(pool, source));
}

TEST(ExtMessagePoolScheduler, ExpiredNativeHeadCleanupPrunesEntirePendingSuffix) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(91);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto expired_head = ExtMessagePoolTestAccess::add(pool, source, 0);
  auto suffix_one = ExtMessagePoolTestAccess::add(pool, source, 1);
  auto suffix_two = ExtMessagePoolTestAccess::add(pool, source, 2);

  ExtMessagePoolTestAccess::expire(pool, expired_head);
  pool.cleanup_external_messages({basechainId, shardIdAll});

  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, expired_head));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, suffix_one));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, suffix_two));
  ASSERT_TRUE(ExtMessagePoolTestAccess::reservation_nonces(pool, source).empty());
  ASSERT_EQ(ExtMessagePoolTestAccess::expiry_suffix_events(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::expiry_suffix_pruned(pool), 3u);
}

TEST(ExtMessagePoolScheduler, CanonicalValidUntilExpiryKeepsPrefixAndPrunesSuffix) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(92);
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 0, 100, 100, 100));
  auto keep = ExtMessagePoolTestAccess::add(pool, source, 0);
  auto expired_middle = ExtMessagePoolTestAccess::add(pool, source, 1);
  auto suffix = ExtMessagePoolTestAccess::add(pool, source, 2);
  ExtMessagePoolTestAccess::set_valid_until(pool, source, 1, 101);

  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 0, 100, 102, 101));

  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, keep));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, expired_middle));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, suffix));
  ASSERT_EQ(ExtMessagePoolTestAccess::reservation_nonces(pool, source), (std::vector<td::uint64>{0}));
  ASSERT_EQ(ExtMessagePoolTestAccess::expiry_suffix_events(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::expiry_suffix_pruned(pool), 2u);
  auto ready = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 8);
  ASSERT_EQ(ready.nonces, (std::vector<td::uint64>{0}));
  ASSERT_EQ(ready.head_gaps, 0u);
}

TEST(ExtMessagePoolScheduler, CommitTimeExpiryRejectsAdmissionAndPrunesSuffix) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(93);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto expired_head = ExtMessagePoolTestAccess::add(pool, source, 0, 0, true, false);
  auto suffix = ExtMessagePoolTestAccess::add(pool, source, 1);
  ExtMessagePoolTestAccess::set_valid_until(pool, source, 0, 100);
  ExtMessagePoolTestAccess::set_native_observed_utime(pool, source, 101);

  ASSERT_TRUE(!ExtMessagePoolTestAccess::finalize_existing_native(pool, source, 0, expired_head));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, expired_head));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, suffix));
  ASSERT_TRUE(ExtMessagePoolTestAccess::reservation_nonces(pool, source).empty());
  ASSERT_EQ(ExtMessagePoolTestAccess::expiry_suffix_events(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::expiry_suffix_pruned(pool), 2u);
}

TEST(ExtMessagePoolScheduler, MissingHeadBlocksFutureNonces) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(1);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  ExtMessagePoolTestAccess::add(pool, source, 1);
  ExtMessagePoolTestAccess::add(pool, source, 2);

  auto blocked = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_TRUE(blocked.nonces.empty());
  ASSERT_EQ(blocked.head_gaps, 1u);
  ASSERT_EQ(blocked.head_missing_nonce, 1u);

  ExtMessagePoolTestAccess::add(pool, source, 0);
  auto ready = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_EQ(ready.nonces, (std::vector<td::uint64>{0, 1, 2}));
  ASSERT_EQ(ready.head_gaps, 0u);
}

TEST(ExtMessagePoolScheduler, InactiveHeadReactivationReleasesRun) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(2);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto head = ExtMessagePoolTestAccess::add(pool, source, 0, 0, false);
  ExtMessagePoolTestAccess::add(pool, source, 1);

  auto inactive = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_TRUE(inactive.nonces.empty());
  ASSERT_EQ(inactive.inactive, 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::make_due_and_reactivate(pool, head), 1u);

  auto ready = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_EQ(ready.nonces, (std::vector<td::uint64>{0, 1}));
}

TEST(ExtMessagePoolScheduler, RotatesBoundedSourceRunsFairly) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source_a = ExtMessagePoolTestAccess::source(1);
  auto source_b = ExtMessagePoolTestAccess::source(2);
  auto source_c = ExtMessagePoolTestAccess::source(3);
  for (auto source : {source_a, source_b, source_c}) {
    ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
    for (td::uint64 nonce = 0; nonce < 32; ++nonce) {
      ExtMessagePoolTestAccess::add(pool, source, nonce);
    }
  }

  auto first = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_EQ(first.sources.size(), 32u);
  ASSERT_TRUE(std::all_of(first.sources.begin(), first.sources.begin() + 16,
                          [&](const auto &source) { return source == source_a; }));
  ASSERT_TRUE(std::all_of(first.sources.begin() + 16, first.sources.end(),
                          [&](const auto &source) { return source == source_b; }));
  ASSERT_EQ(first.max_run_size, 16u);

  auto second = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32, first.cursor);
  ASSERT_TRUE(std::all_of(second.sources.begin(), second.sources.begin() + 16,
                          [&](const auto &source) { return source == source_c; }));
  ASSERT_TRUE(std::all_of(second.sources.begin() + 16, second.sources.end(),
                          [&](const auto &source) { return source == source_a; }));
}

TEST(ExtMessagePoolScheduler, NativeWorkIntervalIsAtomicAndUsesLogicalCapacity) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(94);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto interval = ExtMessagePoolTestAccess::add_work(pool, source, 0, 3);
  ExtMessagePoolTestAccess::add(pool, source, 3);

  // The work is one BOC but represents three contiguous nonce slots. A
  // candidate that only has two logical slots left must not split it.
  auto too_small = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 2);
  ASSERT_TRUE(too_small.nonces.empty());
  ASSERT_EQ(too_small.selected, 0u);
  ASSERT_EQ(too_small.logical_selected, 0u);

  auto ready = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 4);
  ASSERT_EQ(ready.nonces, (std::vector<td::uint64>{0, 3}));
  ASSERT_EQ(ready.logical_counts, (std::vector<td::uint32>{3, 1}));
  ASSERT_EQ(ready.selected, 2u);
  ASSERT_EQ(ready.logical_selected, 4u);

  // Excluding an atomic work advances the speculative source view by the full
  // interval, not merely its first nonce.
  auto excluded = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 4, {}, {interval});
  ASSERT_EQ(excluded.nonces, (std::vector<td::uint64>{3}));
  ASSERT_EQ(excluded.logical_counts, (std::vector<td::uint32>{1}));
  ASSERT_EQ(excluded.excluded, 1u);
}

TEST(ExtMessagePoolScheduler, OversizedNativeWorkCanUseAnEmptySourceRun) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(99);
  const auto logical_count = ExtMessagePoolTestAccess::native_source_run_target() + 1;
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  ExtMessagePoolTestAccess::add_work(pool, source, 0, logical_count);

  // Fairness may stop a source after its target-sized logical run, but a
  // single atomic work is allowed to exceed that soft target. Otherwise a
  // valid interval larger than 16 could never be selected by any candidate.
  auto ready = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, logical_count);
  ASSERT_EQ(ready.nonces, (std::vector<td::uint64>{0}));
  ASSERT_EQ(ready.logical_counts, (std::vector<td::uint32>{logical_count}));
  ASSERT_EQ(ready.selected, 1u);
  ASSERT_EQ(ready.logical_selected, logical_count);
}

TEST(ExtMessagePoolScheduler, CanonicalAdvanceThroughNativeWorkDropsWholeAtomicSuffix) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(95);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto interval = ExtMessagePoolTestAccess::add_work(pool, source, 0, 4);
  auto suffix = ExtMessagePoolTestAccess::add(pool, source, 4);

  // Canonical state consumed nonces 0 and 1. The signed range 0..3 cannot be
  // split into an unsigned suffix, so it and all following reservations are
  // removed instead of leaving a nonce hole.
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 2));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, interval));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, suffix));
  ASSERT_TRUE(ExtMessagePoolTestAccess::reservation_nonces(pool, source).empty());
}

TEST(ExtMessagePoolScheduler, NativeWorkReservesAggregateDebitWithoutPartialInterval) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(96);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto interval = ExtMessagePoolTestAccess::add_work(pool, source, 0, 3, 0, true, true, 10, 2);
  auto suffix = ExtMessagePoolTestAccess::add(pool, source, 3, 0, true, true, 5, 1);

  // NativeWork.amount/fee are aggregate debit totals for all its logical
  // outputs. A caller asking for a boundary inside the interval receives the
  // overflow sentinel rather than a fictional partial reservation.
  ASSERT_EQ(ExtMessagePoolTestAccess::reserved_amount_before(pool, source, 0, 3), 12u);
  ASSERT_EQ(ExtMessagePoolTestAccess::reserved_amount_before(pool, source, 0, 4), 18u);
  ASSERT_EQ(ExtMessagePoolTestAccess::reserved_amount_before(pool, source, 0, 2),
            std::numeric_limits<td::uint64>::max());

  // With only the interval's aggregate debit available, canonical rebasing
  // preserves that atomic work and removes the unaffordable suffix.
  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 0, 12));
  ASSERT_TRUE(ExtMessagePoolTestAccess::contains(pool, interval));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, suffix));
  ASSERT_EQ(ExtMessagePoolTestAccess::reservation_nonces(pool, source), (std::vector<td::uint64>{0}));
}

TEST(ExtMessagePoolScheduler, CallbackSchedulerKeepsNativeWorkAtomicAcrossLogicalWindow) {
  auto too_small_pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(98);
  ExtMessagePoolTestAccess::set_watermark(too_small_pool, source, 0);
  ExtMessagePoolTestAccess::add_work(too_small_pool, source, 0, 3);
  ExtMessagePoolTestAccess::install_live_waiting_callback(too_small_pool, 2);
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(too_small_pool), 0u);
  ASSERT_TRUE(ExtMessagePoolTestAccess::callback_delivery_logical_counts(too_small_pool).empty());
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_selected_ahead(too_small_pool), 0u);

  auto pool = ExtMessagePoolTestAccess::make_pool();
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  ExtMessagePoolTestAccess::add_work(pool, source, 0, 3);
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, 3);
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_delivery_logical_counts(pool), (std::vector<td::uint32>{3}));
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_selected_ahead(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_logical_selected_ahead(pool), 3u);
}

TEST(ExtMessagePoolScheduler, ExclusionsAdvanceOnlySpeculativeView) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(4);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto nonce_zero = ExtMessagePoolTestAccess::add(pool, source, 0);
  ExtMessagePoolTestAccess::add(pool, source, 1);
  ExtMessagePoolTestAccess::add(pool, source, 2);

  auto child = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32, {}, {nonce_zero});
  ASSERT_EQ(child.nonces, (std::vector<td::uint64>{1, 2}));
  ASSERT_EQ(child.excluded, 1u);

  auto losing_fork = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 32);
  ASSERT_EQ(losing_fork.nonces, (std::vector<td::uint64>{0, 1, 2}));
}

TEST(ExtMessagePoolScheduler, CallbackNonceFloorIsBranchLocalAndSiblingReusable) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(0x24);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  for (td::uint64 nonce = 0; nonce < 4; ++nonce) {
    ExtMessagePoolTestAccess::add(pool, source, nonce);
  }

  auto child = ExtMessagePoolTestAccess::select_callback_round(
      pool, {basechainId, shardIdAll}, 4,
      {{.workchain = source.first, .source = source.second, .next_nonce = 2}});
  ASSERT_EQ(child.nonces, (std::vector<td::uint64>{2, 3}));

  // Selection advances only the callback-local scheduler. Neither the global
  // canonical watermark nor the reservations are changed, so a sibling fork
  // starting from the same canonical parent can reuse the complete prefix.
  ASSERT_EQ(ExtMessagePoolTestAccess::first_unconsumed_nonce(pool, source), td::optional<td::uint64>(0));
  ASSERT_EQ(ExtMessagePoolTestAccess::reservation_nonces(pool, source),
            (std::vector<td::uint64>{0, 1, 2, 3}));
  auto sibling = ExtMessagePoolTestAccess::select_callback_round(pool, {basechainId, shardIdAll}, 4);
  ASSERT_EQ(sibling.nonces, (std::vector<td::uint64>{0, 1, 2, 3}));
}

TEST(ExtMessagePoolScheduler, CallbackNonceFloorSkipsAncestorScansWithoutConsumingSiblingWork) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(0x29);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  std::vector<ExtMessage::Hash> ancestor_hashes;
  for (td::uint64 nonce = 0; nonce < 40; ++nonce) {
    auto hash = ExtMessagePoolTestAccess::add(pool, source, nonce);
    if (nonce < 32) {
      ancestor_hashes.push_back(hash);
    }
  }

  // Both callbacks describe the same exact parent. The floor elides the
  // ancestor prefix entirely while exclusions still guard message identity.
  auto control = ExtMessagePoolTestAccess::select_callback_round(
      pool, {basechainId, shardIdAll}, 8, {}, ancestor_hashes);
  auto treatment = ExtMessagePoolTestAccess::select_callback_round(
      pool, {basechainId, shardIdAll}, 8,
      {{.workchain = source.first, .source = source.second, .next_nonce = 32}}, ancestor_hashes);
  ASSERT_EQ(treatment.nonces, control.nonces);
  ASSERT_EQ(treatment.nonces, (std::vector<td::uint64>{32, 33, 34, 35, 36, 37, 38, 39}));
  ASSERT_EQ(control.excluded, 32u);
  ASSERT_EQ(treatment.excluded, 0u);
  ASSERT_EQ(control.scanned - treatment.scanned, 32u);

  // A competing parent has consumed only nonce zero for this same source.
  // Neither the larger sibling floor nor its delivered suffix may leak here.
  auto sibling = ExtMessagePoolTestAccess::select_callback_round(
      pool, {basechainId, shardIdAll}, 3,
      {{.workchain = source.first, .source = source.second, .next_nonce = 1}});
  ASSERT_EQ(sibling.nonces, (std::vector<td::uint64>{1, 2, 3}));
  ASSERT_EQ(ExtMessagePoolTestAccess::first_unconsumed_nonce(pool, source), td::optional<td::uint64>(0));
  ASSERT_EQ(ExtMessagePoolTestAccess::reservation_nonces(pool, source).size(), 40u);
}

TEST(ExtMessagePoolScheduler, CallbackNonceFloorUsesMaxOfCanonicalAndNormalizedBranchFloor) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(0x25);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 2);
  for (td::uint64 nonce = 2; nonce < 6; ++nonce) {
    ExtMessagePoolTestAccess::add(pool, source, nonce);
  }

  // Deliberately unsorted duplicate source entries prove normalization keeps
  // the maximum branch floor; it then wins over the lower canonical value.
  auto branch_ahead = ExtMessagePoolTestAccess::select_callback_round(
      pool, {basechainId, shardIdAll}, 4,
      {{.workchain = source.first, .source = source.second, .next_nonce = 1},
       {.workchain = source.first, .source = source.second, .next_nonce = 4},
       {.workchain = source.first, .source = source.second, .next_nonce = 3}});
  ASSERT_EQ(branch_ahead.nonces, (std::vector<td::uint64>{4, 5}));

  auto canonical_ahead = ExtMessagePoolTestAccess::select_callback_round(
      pool, {basechainId, shardIdAll}, 4,
      {{.workchain = source.first, .source = source.second, .next_nonce = 1}});
  ASSERT_EQ(canonical_ahead.nonces, (std::vector<td::uint64>{2, 3, 4, 5}));
}

TEST(ExtMessagePoolScheduler, CallbackNonceFloorDoesNotRequireAncestorReservations) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(0x26);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  ExtMessagePoolTestAccess::add(pool, source, 5);
  ExtMessagePoolTestAccess::add(pool, source, 6);

  // The exact parent state is the authority for the skipped prefix. A node
  // that learned the parent from a peer need not have reservations 0..4.
  auto selected = ExtMessagePoolTestAccess::select_callback_round(
      pool, {basechainId, shardIdAll}, 8,
      {{.workchain = source.first, .source = source.second, .next_nonce = 5}});
  ASSERT_EQ(selected.nonces, (std::vector<td::uint64>{5, 6}));
  ASSERT_EQ(ExtMessagePoolTestAccess::first_unconsumed_nonce(pool, source), td::optional<td::uint64>(0));
}

TEST(ExtMessagePoolScheduler, TargetedCallbackRefreshRetainsBranchNonceFloor) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(0x27);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto ancestor = ExtMessagePoolTestAccess::add(pool, source, 0);
  ExtMessagePoolTestAccess::install_live_waiting_callback(
      pool, ExtMessagePoolTestAccess::max_native_queue_limit(), 500, {}, {basechainId, shardIdAll},
      {{.workchain = source.first, .source = source.second, .next_nonce = 2}});

  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), 0u);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::callback_contains_delivery(pool, ancestor));
  auto next = ExtMessagePoolTestAccess::add(pool, source, 2);
  ASSERT_EQ(ExtMessagePoolTestAccess::wake_sources(pool, {source}), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::dirty_sources(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::resume_native_pump_refill(pool), 1u);
  ASSERT_TRUE(ExtMessagePoolTestAccess::callback_has_delivery(pool, next));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::callback_contains_delivery(pool, ancestor));
}

TEST(ExtMessagePoolScheduler, CallbackNonceFloorKeepsSourceSignedRunsAtomic) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(0x28);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  ExtMessagePoolTestAccess::add_work(pool, source, 0, 3);
  ExtMessagePoolTestAccess::add(pool, source, 3);

  auto child = ExtMessagePoolTestAccess::select_callback_round(
      pool, {basechainId, shardIdAll}, 4,
      {{.workchain = source.first, .source = source.second, .next_nonce = 3}});
  ASSERT_EQ(child.nonces, (std::vector<td::uint64>{3}));
  ASSERT_EQ(child.logical_counts, (std::vector<td::uint32>{1}));

  auto sibling = ExtMessagePoolTestAccess::select_callback_round(pool, {basechainId, shardIdAll}, 4);
  ASSERT_EQ(sibling.nonces, (std::vector<td::uint64>{0, 3}));
  ASSERT_EQ(sibling.logical_counts, (std::vector<td::uint32>{3, 1}));

  // A malformed/intermediate floor may not synthesize a suffix of the one
  // physical run or skip across that incomplete interval.
  auto interior = ExtMessagePoolTestAccess::select_callback_round(
      pool, {basechainId, shardIdAll}, 4,
      {{.workchain = source.first, .source = source.second, .next_nonce = 2}});
  ASSERT_TRUE(interior.nonces.empty());
}

TEST(ExtMessagePoolScheduler, CallbackNonceFloorsAreShardIsolated) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  const ShardIdFull left_shard{basechainId, shard_child(shardIdAll, true)};
  const ShardIdFull right_shard{basechainId, shard_child(shardIdAll, false)};
  auto left_source = ExtMessagePoolTestAccess::source(0x10);
  auto right_source = ExtMessagePoolTestAccess::source(0x90);
  ExtMessagePoolTestAccess::set_watermark(pool, left_source, 0);
  ExtMessagePoolTestAccess::set_watermark(pool, right_source, 0);
  auto left_zero = ExtMessagePoolTestAccess::add(pool, left_source, 0);
  auto left_one = ExtMessagePoolTestAccess::add(pool, left_source, 1);
  ExtMessagePoolTestAccess::add(pool, right_source, 0);
  ExtMessagePoolTestAccess::add(pool, right_source, 1);

  ExtMessagePoolTestAccess::install_live_waiting_callback(
      pool, ExtMessagePoolTestAccess::max_native_queue_limit(), 500, {}, left_shard,
      {{.workchain = right_source.first, .source = right_source.second, .next_nonce = 1},
       {.workchain = left_source.first, .source = left_source.second, .next_nonce = 1}});
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_native_floor_sources(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), 1u);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::callback_contains_delivery(pool, left_zero));
  ASSERT_TRUE(ExtMessagePoolTestAccess::callback_contains_delivery(pool, left_one));

  // The right-lane floor was discarded by the left callback and global state
  // remains untouched, so a right callback starts at its canonical nonce.
  auto right = ExtMessagePoolTestAccess::select_callback_round(pool, right_shard, 2);
  ASSERT_EQ(right.nonces, (std::vector<td::uint64>{0, 1}));
}

TEST(ExtMessagePoolScheduler, MasterchainNeverScansNativeSources) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(5);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  for (td::uint64 nonce = 0; nonce < 32; ++nonce) {
    ExtMessagePoolTestAccess::add(pool, source, nonce);
  }

  auto selected = ExtMessagePoolTestAccess::select(pool, {masterchainId, shardIdAll}, 500);
  ASSERT_TRUE(selected.nonces.empty());
  ASSERT_EQ(selected.scanned, 0u);
  ASSERT_EQ(selected.selected, 0u);
}

TEST(ExtMessagePoolScheduler, NativeLeafCallbacksScanAndRotateOnlyWithinTheirShard) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  const ShardIdFull left_shard{basechainId, shard_child(shardIdAll, true)};
  const ShardIdFull right_shard{basechainId, shard_child(shardIdAll, false)};
  auto left_a = ExtMessagePoolTestAccess::source(0x10);
  auto left_b = ExtMessagePoolTestAccess::source(0x20);
  auto right_a = ExtMessagePoolTestAccess::source(0x90);
  auto right_b = ExtMessagePoolTestAccess::source(0xa0);
  for (const auto &source : {left_a, left_b, right_a, right_b}) {
    ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
    ExtMessagePoolTestAccess::add(pool, source, 0);
  }

  auto left_first = ExtMessagePoolTestAccess::select_callback_round(pool, left_shard, 1);
  auto right_first = ExtMessagePoolTestAccess::select_callback_round(pool, right_shard, 1);
  auto left_second = ExtMessagePoolTestAccess::select_callback_round(pool, left_shard, 1);
  auto right_second = ExtMessagePoolTestAccess::select_callback_round(pool, right_shard, 1);

  ASSERT_EQ(left_first.source_scans, 2u);
  ASSERT_EQ(right_first.source_scans, 2u);
  ASSERT_EQ(left_second.source_scans, 2u);
  ASSERT_EQ(right_second.source_scans, 2u);
  ASSERT_EQ(left_first.sources.size(), 1u);
  ASSERT_EQ(right_first.sources.size(), 1u);
  ASSERT_EQ(left_second.sources.size(), 1u);
  ASSERT_EQ(right_second.sources.size(), 1u);
  ASSERT_EQ(left_first.sources.front(), left_a);
  ASSERT_EQ(right_first.sources.front(), right_a);
  ASSERT_EQ(left_second.sources.front(), left_b);
  ASSERT_EQ(right_second.sources.front(), right_b);
}

TEST(ExtMessagePoolScheduler, NativeIngressWakesOnlyItsLeafCallback) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  const ShardIdFull left_shard{basechainId, shard_child(shardIdAll, true)};
  const ShardIdFull right_shard{basechainId, shard_child(shardIdAll, false)};
  auto left_source = ExtMessagePoolTestAccess::source(0x10);
  auto right_source = ExtMessagePoolTestAccess::source(0x90);
  ExtMessagePoolTestAccess::install_live_waiting_callback(
      pool, ExtMessagePoolTestAccess::max_native_queue_limit(), 500, {}, left_shard);
  ExtMessagePoolTestAccess::install_live_waiting_callback(
      pool, ExtMessagePoolTestAccess::max_native_queue_limit(), 500, {}, right_shard);

  ASSERT_EQ(ExtMessagePoolTestAccess::wake_sources(pool, {left_source, right_source}), 2u);
  ASSERT_EQ(ExtMessagePoolTestAccess::dirty_sources(pool, 0), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::dirty_sources(pool, 1), 1u);
  ASSERT_TRUE(ExtMessagePoolTestAccess::has_dirty_source(pool, 0, left_source));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_dirty_source(pool, 0, right_source));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_dirty_source(pool, 1, left_source));
  ASSERT_TRUE(ExtMessagePoolTestAccess::has_dirty_source(pool, 1, right_source));
}

TEST(ExtMessagePoolScheduler, NativeDepthTwoCallbacksScanOnlyTheirQuarterShard) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  const auto left = shard_child(shardIdAll, true);
  const auto right = shard_child(shardIdAll, false);
  const std::array<ShardIdFull, 4> shards{
      ShardIdFull{basechainId, shard_child(left, true)},
      ShardIdFull{basechainId, shard_child(left, false)},
      ShardIdFull{basechainId, shard_child(right, true)},
      ShardIdFull{basechainId, shard_child(right, false)},
  };
  const std::array<ExtMessagePoolTestAccess::NativeAddress, 4> sources{
      ExtMessagePoolTestAccess::source(0x10),
      ExtMessagePoolTestAccess::source(0x50),
      ExtMessagePoolTestAccess::source(0x90),
      ExtMessagePoolTestAccess::source(0xd0),
  };
  for (const auto &source : sources) {
    ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
    ExtMessagePoolTestAccess::add(pool, source, 0);
  }

  for (std::size_t lane = 0; lane < shards.size(); ++lane) {
    auto selected = ExtMessagePoolTestAccess::select_callback_round(pool, shards[lane], 1);
    ASSERT_EQ(selected.source_scans, 1u);
    ASSERT_EQ(selected.sources, (std::vector<ExtMessagePoolTestAccess::NativeAddress>{sources[lane]}));
  }
}

TEST(ExtMessagePoolScheduler, PostCommitWakesLiveWaiterWithoutNewIngress) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(6);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto head = ExtMessagePoolTestAccess::add(pool, source, 0, 0, true, false);

  auto before_commit = ExtMessagePoolTestAccess::select(pool, {basechainId, shardIdAll}, 1);
  ASSERT_TRUE(before_commit.nonces.empty());
  ASSERT_EQ(before_commit.head_gaps, 1u);
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());

  ASSERT_TRUE(ExtMessagePoolTestAccess::finalize_existing_native(pool, source, 0, head));
  ASSERT_TRUE(ExtMessagePoolTestAccess::reservation_committed(pool, source, 0));
  ASSERT_EQ(ExtMessagePoolTestAccess::dirty_sources(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::resume_native_pump_refill(pool), 1u);
  ASSERT_TRUE(ExtMessagePoolTestAccess::callback_has_delivery(pool, head));
}

TEST(ExtMessagePoolScheduler, NativeDirectLinksMatchLegacyCallbackSelection) {
  auto configure = [](ExtMessagePool &pool, bool link_direct) {
    auto source_a = ExtMessagePoolTestAccess::source(60);
    auto source_b = ExtMessagePoolTestAccess::source(61);
    ExtMessagePoolTestAccess::set_watermark(pool, source_a, 0);
    ExtMessagePoolTestAccess::set_watermark(pool, source_b, 0);
    auto a0 = ExtMessagePoolTestAccess::add(pool, source_a, 0, 4, true, true, 1, 0, link_direct);
    auto a1 = ExtMessagePoolTestAccess::add(pool, source_a, 1, 1, true, true, 1, 0, link_direct);
    auto b0 = ExtMessagePoolTestAccess::add(pool, source_b, 0, 7, true, true, 1, 0, link_direct);
    auto b1 = ExtMessagePoolTestAccess::add(pool, source_b, 1, 0, true, true, 1, 0, link_direct);
    return std::vector<ExtMessage::Hash>{a0, a1, b0, b1};
  };

  auto linked = ExtMessagePoolTestAccess::make_pool();
  auto legacy = ExtMessagePoolTestAccess::make_pool();
  auto expected = configure(linked, true);
  configure(legacy, false);
  ExtMessagePoolTestAccess::install_live_waiting_callback(linked,
                                                          ExtMessagePoolTestAccess::max_native_queue_limit());
  ExtMessagePoolTestAccess::install_live_waiting_callback(legacy,
                                                          ExtMessagePoolTestAccess::max_native_queue_limit());

  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(linked), expected.size());
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(legacy), expected.size());
  const std::vector<ExtMessage::Hash> priority_order{expected[2], expected[0], expected[1], expected[3]};
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_delivery_hashes(linked), priority_order);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_delivery_hashes(legacy), priority_order);

  auto linked_stats = ExtMessagePoolTestAccess::scheduler_stats(linked);
  auto legacy_stats = ExtMessagePoolTestAccess::scheduler_stats(legacy);
  ASSERT_TRUE(linked_stats.direct_link_hits > 0);
  ASSERT_EQ(linked_stats.direct_link_fallbacks, 0u);
  ASSERT_TRUE(legacy_stats.direct_link_hits > 0);
  ASSERT_TRUE(legacy_stats.direct_link_fallbacks >= 2u);
}

TEST(ExtMessagePoolScheduler, NativeDirectLinkPreservesExclusionPriorityAndInactiveHeadSemantics) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source_a = ExtMessagePoolTestAccess::source(62);
  auto source_b = ExtMessagePoolTestAccess::source(63);
  ExtMessagePoolTestAccess::set_watermark(pool, source_a, 0);
  ExtMessagePoolTestAccess::set_watermark(pool, source_b, 0);
  auto excluded = ExtMessagePoolTestAccess::add(pool, source_a, 0, 4);
  auto after_excluded = ExtMessagePoolTestAccess::add(pool, source_a, 1, 5);
  auto high_priority = ExtMessagePoolTestAccess::add(pool, source_b, 0, 7);
  auto inactive_tail = ExtMessagePoolTestAccess::add(pool, source_b, 1, 9, false);

  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit(),
                                                          500, {excluded});
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), 2u);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_delivery_hashes(pool),
            (std::vector<ExtMessage::Hash>{high_priority, after_excluded}));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::callback_contains_delivery(pool, inactive_tail));
  auto stats = ExtMessagePoolTestAccess::scheduler_stats(pool);
  ASSERT_TRUE(stats.direct_link_hits > 0);
  ASSERT_EQ(stats.direct_link_fallbacks, 0u);
  ASSERT_EQ(stats.excluded, 1u);
  ASSERT_EQ(stats.inactive, 1u);
}

TEST(ExtMessagePoolScheduler, NativeDirectLinkFallbackRepairsMissingAndStaleLinks) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(64);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto first = ExtMessagePoolTestAccess::add(pool, source, 0);
  auto second = ExtMessagePoolTestAccess::add(pool, source, 1);
  ExtMessagePoolTestAccess::clear_native_mempool_link(pool, source, 0);
  ExtMessagePoolTestAccess::mark_native_mempool_link_stale(pool, source, 1);

  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), 2u);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_delivery_hashes(pool),
            (std::vector<ExtMessage::Hash>{first, second}));
  ASSERT_TRUE(ExtMessagePoolTestAccess::has_native_mempool_link(pool, source, 0));
  ASSERT_TRUE(ExtMessagePoolTestAccess::has_native_mempool_link(pool, source, 1));
  auto stats = ExtMessagePoolTestAccess::scheduler_stats(pool);
  ASSERT_TRUE(stats.direct_link_hits > 0);
  ASSERT_TRUE(stats.direct_link_fallbacks >= 2u);
}

TEST(ExtMessagePoolScheduler, StaleNativeDirectLinkCannotBypassLegacyHeadGap) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(65);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto hash = ExtMessagePoolTestAccess::add(pool, source, 0);
  ExtMessagePoolTestAccess::set_reservation_hash(pool, source, 0, make_bits(900, 65));

  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), 0u);
  ASSERT_TRUE(!ExtMessagePoolTestAccess::callback_contains_delivery(pool, hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_native_mempool_link(pool, source, 0));
  auto stats = ExtMessagePoolTestAccess::scheduler_stats(pool);
  ASSERT_EQ(stats.direct_link_hits, 0u);
  ASSERT_EQ(stats.direct_link_fallbacks, 1u);
  ASSERT_EQ(stats.head_gaps, 1u);
}

TEST(ExtMessagePoolScheduler, CanonicalReconciliationReleasesNativeDirectLinkBeforeCallbackFill) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(67);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto hash = ExtMessagePoolTestAccess::add(pool, source, 0);
  auto weak_message = ExtMessagePoolTestAccess::native_mempool_weak(pool, source, 0);

  ASSERT_TRUE(ExtMessagePoolTestAccess::reconcile_account(pool, source, 1, 100, 101, 101));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_native_reservation(pool, source, 0));
  ASSERT_TRUE(weak_message.expired());

  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), 0u);
}

TEST(ExtMessagePoolScheduler, ErasingRealPoolObjectInvalidatesMismatchedNativeDirectLink) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(68);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto hash = ExtMessagePoolTestAccess::add(pool, source, 0);
  auto weak_message = ExtMessagePoolTestAccess::native_mempool_weak(pool, source, 0);
  ExtMessagePoolTestAccess::set_reservation_hash(pool, source, 0, make_bits(901, 68));

  // The mismatched reservation deliberately survives the raw-hash erase so
  // the callback has to prove it cannot follow the old direct pointer.
  pool.complete_external_messages({}, {hash});
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, hash));
  ASSERT_TRUE(ExtMessagePoolTestAccess::has_native_reservation(pool, source, 0));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_native_mempool_link(pool, source, 0));
  ASSERT_TRUE(weak_message.expired());

  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), 0u);
  auto stats = ExtMessagePoolTestAccess::scheduler_stats(pool);
  ASSERT_EQ(stats.direct_link_hits, 0u);
  ASSERT_EQ(stats.direct_link_fallbacks, 1u);
  ASSERT_EQ(stats.head_gaps, 1u);
}

TEST(ExtMessagePoolScheduler, NativeDirectLinkDoesNotRetainMempoolObjectAfterCancellationAndErase) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source = ExtMessagePoolTestAccess::source(66);
  ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
  auto hash = ExtMessagePoolTestAccess::add(pool, source, 0);
  auto weak_message = ExtMessagePoolTestAccess::native_mempool_weak(pool, source, 0);

  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), 1u);
  ExtMessagePoolTestAccess::cancel_callback(pool);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_pending(pool), 0u);

  pool.complete_external_messages({}, {hash});
  ASSERT_TRUE(!ExtMessagePoolTestAccess::contains(pool, hash));
  ASSERT_TRUE(!ExtMessagePoolTestAccess::has_native_reservation(pool, source, 0));
  ASSERT_TRUE(weak_message.expired());
}

TEST(ExtMessagePoolScheduler, CallbackSelectionIsIncrementalAndChunkBounded) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  for (unsigned source_id = 1; source_id <= 40; ++source_id) {
    auto source = ExtMessagePoolTestAccess::source(source_id);
    ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
    for (td::uint64 nonce = 0; nonce < 16; ++nonce) {
      ExtMessagePoolTestAccess::add(pool, source, nonce);
    }
  }
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());

  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_pending(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
}

TEST(ExtMessagePoolScheduler, NativeTransportPrefillSeedsWindowAndBoundsProducerLookAhead) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  constexpr std::size_t transport_window = 2'048;
  constexpr unsigned source_count = 40;
  constexpr td::uint64 nonces_per_source = 64;
  static_assert(source_count * nonces_per_source > transport_window);
  for (unsigned source_id = 1; source_id <= source_count; ++source_id) {
    auto source = ExtMessagePoolTestAccess::source(source_id);
    ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
    for (td::uint64 nonce = 0; nonce < nonces_per_source; ++nonce) {
      ExtMessagePoolTestAccess::add(pool, source, nonce);
    }
  }
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool,
                                                          ExtMessagePoolTestAccess::max_native_queue_limit(),
                                                          transport_window);

  // Installation selects the whole configured transport window in fair
  // 512-message scheduler chunks before the producer begins to push it.
  ASSERT_EQ(ExtMessagePoolTestAccess::prefill_native_transport(pool), transport_window);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_pending(pool), transport_window);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_selected_ahead(pool), transport_window);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_native_transport_selected_limit(pool),
            transport_window + 2 * ExtMessagePoolTestAccess::native_delivery_chunk());
  ASSERT_TRUE(ExtMessagePoolTestAccess::callback_native_transport_has_refill_credit(pool));

  // The hard hand-off bound permits at most two additional scheduler
  // fragments. The low-watermark consumption gate is covered separately;
  // this assertion protects the global selected-ahead cap itself.
  ExtMessagePoolTestAccess::record_callback_selected(pool, ExtMessagePoolTestAccess::native_delivery_chunk());
  ASSERT_TRUE(ExtMessagePoolTestAccess::callback_native_transport_has_refill_credit(pool));
  ExtMessagePoolTestAccess::record_callback_selected(pool, ExtMessagePoolTestAccess::native_delivery_chunk());
  ASSERT_TRUE(!ExtMessagePoolTestAccess::callback_native_transport_has_refill_credit(pool));
}

TEST(ExtMessagePoolScheduler, NativeTransportLowWatermarkDefersSecondBackupAndFragmentsPublication) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  constexpr std::size_t transport_window = 2'048;
  constexpr unsigned source_count = 64;
  constexpr td::uint64 nonces_per_source = 64;
  static_assert(source_count * nonces_per_source > transport_window + 2 * 512);
  for (unsigned source_id = 1; source_id <= source_count; ++source_id) {
    auto source = ExtMessagePoolTestAccess::source(source_id);
    ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
    for (td::uint64 nonce = 0; nonce < nonces_per_source; ++nonce) {
      ExtMessagePoolTestAccess::add(pool, source, nonce);
    }
  }
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool,
                                                          ExtMessagePoolTestAccess::max_native_queue_limit(),
                                                          transport_window);

  const auto fragment = ExtMessagePoolTestAccess::native_delivery_chunk();
  ASSERT_EQ(ExtMessagePoolTestAccess::prefill_native_transport(pool), transport_window);
  // The known-good installation hand-off may fill the physical window in one
  // request. It is explicitly distinct from all later low-watermark pushes.
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_native_transport_publish_batch_capacity(pool), transport_window);
  ExtMessagePoolTestAccess::publish_callback_pending_native(pool);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_selected_ahead(pool), transport_window);
  const auto scheduler_after_initial = ExtMessagePoolTestAccess::scheduler_stats(pool);

  // Before a full fragment has been consumed, only the existing one-fragment
  // look-ahead may be selected. Repeating the refill cannot create the
  // rejected two-fragment eager prefix.
  ASSERT_EQ(ExtMessagePoolTestAccess::prefill_native_transport_low_watermark(pool), fragment);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_pending(pool), fragment);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_selected_ahead(pool), transport_window + fragment);
  ASSERT_EQ(ExtMessagePoolTestAccess::prefill_native_transport_low_watermark(pool), 0u);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_native_transport_publish_batch_capacity(pool), fragment);

  // Once the Collator has made a full fragment of progress, the second backup
  // is eligible. Even with two fragments staged, every post-initial queue
  // operation remains exactly one fair 512-message fragment.
  ExtMessagePoolTestAccess::consume_callback_native(pool, fragment);
  ExtMessagePoolTestAccess::publish_callback_pending_native(pool);
  ASSERT_EQ(ExtMessagePoolTestAccess::prefill_native_transport_low_watermark(pool), 2 * fragment);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_pending(pool), 2 * fragment);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_selected_ahead(pool), transport_window + 2 * fragment);
  ASSERT_EQ(ExtMessagePoolTestAccess::prefill_native_transport_low_watermark(pool), 0u);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_native_transport_publish_batch_capacity(pool), fragment);
  const auto scheduler_after_prefetch = ExtMessagePoolTestAccess::scheduler_stats(pool);
  ASSERT_EQ(scheduler_after_prefetch.builds, scheduler_after_initial.builds);
  ASSERT_EQ(scheduler_after_prefetch.source_scans, scheduler_after_initial.source_scans);

  // Callback-local staging never crosses a cancelled candidate. Exact queue
  // accounting partitions the physical window and the two unpushed backup
  // fragments without a live residue.
  auto telemetry = ExtMessagePoolTestAccess::callback_transport_telemetry(pool);
  ExtMessagePoolTestAccess::cancel_callback(pool);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_pending(pool), 0u);
  ASSERT_EQ(telemetry->queued_discarded.load(), transport_window);
  ASSERT_EQ(telemetry->unpushed_discarded.load(), 2 * fragment);
  ASSERT_EQ(telemetry->queued_discarded.load() + telemetry->unpushed_discarded.load(),
            transport_window + 2 * fragment);
}

TEST(ExtMessagePoolScheduler, PersistentCallbackSchedulerScansSourcesOnlyOnceAcrossChunks) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  constexpr unsigned source_count = 64;
  constexpr td::uint64 nonces_per_source = 32;
  for (unsigned source_id = 1; source_id <= source_count; ++source_id) {
    auto source = ExtMessagePoolTestAccess::source(source_id);
    ExtMessagePoolTestAccess::set_watermark(pool, source, 0);
    for (td::uint64 nonce = 0; nonce < nonces_per_source; ++nonce) {
      ExtMessagePoolTestAccess::add(pool, source, nonce);
    }
  }
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());

  constexpr std::size_t total = source_count * nonces_per_source;
  for (std::size_t selected = 0; selected < total;
       selected += ExtMessagePoolTestAccess::native_delivery_chunk()) {
    ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
  }
  auto stats = ExtMessagePoolTestAccess::scheduler_stats(pool);
  ASSERT_EQ(stats.selected, total);
  ASSERT_EQ(stats.builds, 1u);
  ASSERT_EQ(stats.source_scans, source_count);
  // One initial probe per source, one probe per selected nonce, plus one
  // ready-head revalidation per 16-message source run.
  ASSERT_TRUE(stats.source_probes <= source_count + total + stats.runs + source_count);
}

TEST(ExtMessagePoolScheduler, TargetedRefreshAddsSourceWithoutRebuildingScheduler) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto source_a = ExtMessagePoolTestAccess::source(70);
  ExtMessagePoolTestAccess::set_watermark(pool, source_a, 0);
  ExtMessagePoolTestAccess::add(pool, source_a, 0);
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool, ExtMessagePoolTestAccess::max_native_queue_limit());
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), 1u);

  auto before = ExtMessagePoolTestAccess::scheduler_stats(pool);
  auto source_b = ExtMessagePoolTestAccess::source(71);
  ExtMessagePoolTestAccess::set_watermark(pool, source_b, 0);
  auto source_b_hash = ExtMessagePoolTestAccess::add(pool, source_b, 0);
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_sources(pool, {source_b}), 1u);
  ASSERT_TRUE(ExtMessagePoolTestAccess::callback_contains_delivery(pool, source_b_hash));

  auto after = ExtMessagePoolTestAccess::scheduler_stats(pool);
  ASSERT_EQ(after.builds, before.builds);
  ASSERT_EQ(after.source_scans, before.source_scans);
  ASSERT_EQ(after.source_refreshes, before.source_refreshes + 1);
}

TEST(ExtMessagePoolScheduler, ActivePumpCoalescesIngressWithoutGrowingPendingChunk) {
  auto pool = ExtMessagePoolTestAccess::make_pool();
  auto ready_source = ExtMessagePoolTestAccess::source(72);
  ExtMessagePoolTestAccess::set_watermark(pool, ready_source, 0);
  for (td::uint64 nonce = 0; nonce < 1024; ++nonce) {
    ExtMessagePoolTestAccess::add(pool, ready_source, nonce);
  }
  ExtMessagePoolTestAccess::install_live_waiting_callback(pool,
                                                          ExtMessagePoolTestAccess::max_native_queue_limit());
  ASSERT_EQ(ExtMessagePoolTestAccess::fill_once(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_pending(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
  auto selected_before = ExtMessagePoolTestAccess::scheduler_stats(pool).selected;

  // The callback models a pump suspended on a full transport window. Commits
  // behind its already-valid head must not replace that ready token, and a new
  // source is marked once rather than eagerly appending 512 messages per wake.
  std::set<ExtMessagePoolTestAccess::NativeAddress> ready_sources{ready_source};
  for (unsigned i = 0; i < 1024; ++i) {
    ASSERT_EQ(ExtMessagePoolTestAccess::wake_sources(pool, ready_sources, true), 0u);
  }
  ASSERT_EQ(ExtMessagePoolTestAccess::dirty_sources(pool), 0u);

  auto new_source = ExtMessagePoolTestAccess::source(73);
  ExtMessagePoolTestAccess::set_watermark(pool, new_source, 0);
  ExtMessagePoolTestAccess::add(pool, new_source, 0);
  std::set<ExtMessagePoolTestAccess::NativeAddress> new_sources{new_source};
  std::size_t woken = 0;
  for (unsigned i = 0; i < 1024; ++i) {
    woken += ExtMessagePoolTestAccess::wake_sources(pool, new_sources, true);
  }
  ASSERT_EQ(woken, 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::dirty_sources(pool), 1u);
  ASSERT_EQ(ExtMessagePoolTestAccess::callback_pending(pool), ExtMessagePoolTestAccess::native_delivery_chunk());
  ASSERT_EQ(ExtMessagePoolTestAccess::scheduler_stats(pool).selected, selected_before);
}

TEST(ExtMessagePoolScheduler, CompletionEpochCannotFinishNewerProducerWork) {
  ExtMsgQueueState state;
  auto first = state.begin_producer_epoch();
  ASSERT_TRUE(state.producer_pending());
  state.observe_completion(first);
  ASSERT_TRUE(!state.producer_pending());

  auto second = state.begin_producer_epoch();
  ASSERT_TRUE(second > first);
  state.observe_completion(first);
  ASSERT_TRUE(state.producer_pending());
  state.observe_completion(second);
  ASSERT_TRUE(!state.producer_pending());
}

TEST(ExtMessagePoolScheduler, CompletionMarkerIsFifoAndNotCountedAsTransfer) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<td::Unit> {
    ExtMsgQueue queue("native-transport", 3);
    auto state = std::make_shared<ExtMsgQueueState>();
    auto telemetry = std::make_shared<ExtMsgQueueTelemetry>();
    state->attach_telemetry(telemetry);
    auto epoch = state->begin_producer_epoch();

    auto source = ExtMessagePoolTestAccess::source(9);
    auto first = td::make_ref<FakeExtMessage>(source.second, make_bits(1, 109));
    auto second = td::make_ref<FakeExtMessage>(source.second, make_bits(2, 109));
    state->record_selected(2);
    std::vector<ExtMsgQueueEntry> messages;
    messages.push_back(ExtMsgQueueEntry::make_message({first, 0}, true));
    messages.push_back(ExtMsgQueueEntry::make_message({second, 0}, true));
    auto pushed = co_await queue.push_many_bounded(std::move(messages), 2);
    state->record_pushed(pushed);
    ASSERT_EQ(pushed, 2u);
    auto marker_pushed = co_await queue.push(ExtMsgQueueEntry::make_completion(epoch));
    ASSERT_TRUE(marker_pushed);

    auto entries = co_await queue.pop_many(3);
    ASSERT_EQ(entries.size(), 3u);
    ASSERT_TRUE(entries[0].message);
    ASSERT_TRUE(entries[1].message);
    ASSERT_TRUE(entries[2].is_completion());
    state->record_consumed(2);
    state->observe_completion(entries[2].completed_epoch);
    ASSERT_TRUE(!state->producer_pending());
    ASSERT_EQ(telemetry->selected.load(), 2u);
    ASSERT_EQ(telemetry->pushed.load(), 2u);
    ASSERT_EQ(telemetry->consumed.load(), 2u);
    queue.close();
    co_return td::Unit{};
  });
}

TEST(ExtMessagePoolScheduler, BlockedConsumerMayDrainReservedPushBeforeProducerContinuation) {
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<td::Unit> {
    ExtMsgQueue queue("native-transport-race", 1);
    auto state = std::make_shared<ExtMsgQueueState>();
    auto telemetry = std::make_shared<ExtMsgQueueTelemetry>();
    state->attach_telemetry(telemetry);

    bool consumer_finished = false;
    auto consumer = [](ExtMsgQueue queue, std::shared_ptr<ExtMsgQueueState> state,
                       bool *consumer_finished) -> td::actor::Task<td::Unit> {
      auto entries = co_await queue.pop_many(1);
      ASSERT_EQ(entries.size(), 1u);
      ASSERT_TRUE(entries.front().message);
      state->record_consumed(1);
      *consumer_finished = true;
      co_return td::Unit{};
    }(queue, state, &consumer_finished);
    auto started_consumer = std::move(consumer).start();
    co_await scheduler.wait_sync_work();

    auto source = ExtMessagePoolTestAccess::source(10);
    auto message = td::make_ref<FakeExtMessage>(source.second, make_bits(1, 110));
    std::vector<ExtMsgQueueEntry> batch;
    batch.push_back(ExtMsgQueueEntry::make_message({message, 0}, true));
    state->record_selected(1);
    state->record_push_started(1);
    auto push = queue.push_many_bounded(std::move(batch), 1);

    // Drain all actor work without awaiting the producer result. The queue has
    // woken the blocked pop, but producer-side exact publication has not run.
    co_await scheduler.wait_sync_work();
    ASSERT_TRUE(consumer_finished);
    ASSERT_EQ(telemetry->pushed.load(), 0u);
    ASSERT_EQ(telemetry->push_reserved.load(), 1u);
    ASSERT_EQ(telemetry->consumed.load(), 1u);
    ASSERT_EQ(telemetry->high_water.load(), 1u);

    auto pushed = co_await std::move(push);
    ASSERT_EQ(pushed, 1u);
    state->record_push_completed(1, pushed);
    ASSERT_EQ(telemetry->pushed.load(), 1u);
    ASSERT_EQ(telemetry->push_reserved.load(), 0u);
    ASSERT_EQ(telemetry->consumed.load(), 1u);
    co_await std::move(started_consumer);
    queue.close();
    co_return td::Unit{};
  });
}

TEST(ExtMessagePoolScheduler, CancellationAccountingReclassifiesLatePushAndDrain) {
  auto telemetry = std::make_shared<ExtMsgQueueTelemetry>();
  ExtMsgQueueState state;
  state.attach_telemetry(telemetry);
  state.record_selected(10);
  state.record_pushed(6);
  state.record_consumed(2);
  state.record_cancel_discarded();
  ASSERT_EQ(telemetry->unpushed_discarded.load(), 4u);
  ASSERT_EQ(telemetry->queued_discarded.load(), 4u);
  ASSERT_EQ(telemetry->logical_unpushed_discarded.load(), 4u);
  ASSERT_EQ(telemetry->logical_queued_discarded.load(), 4u);

  // Reserve the complete pump batch before it blocks. Cancellation initially
  // classifies the reservation as queue-owned; exact partial completion moves
  // only the failed suffix back to unpushed-discarded.
  state.record_push_started(4);
  ASSERT_EQ(telemetry->push_reserved.load(), 4u);
  ASSERT_EQ(telemetry->unpushed_discarded.load(), 0u);
  ASSERT_EQ(telemetry->queued_discarded.load(), 8u);
  ASSERT_EQ(telemetry->logical_push_reserved.load(), 4u);
  ASSERT_EQ(telemetry->logical_unpushed_discarded.load(), 0u);
  ASSERT_EQ(telemetry->logical_queued_discarded.load(), 8u);
  state.record_push_completed(4, 2);
  state.record_consumed(3);
  ASSERT_EQ(telemetry->selected.load(), 10u);
  ASSERT_EQ(telemetry->pushed.load(), 8u);
  ASSERT_EQ(telemetry->push_reserved.load(), 0u);
  ASSERT_EQ(telemetry->consumed.load(), 5u);
  ASSERT_EQ(telemetry->unpushed_discarded.load(), 2u);
  ASSERT_EQ(telemetry->queued_discarded.load(), 3u);
  ASSERT_EQ(telemetry->logical_selected.load(), 10u);
  ASSERT_EQ(telemetry->logical_pushed.load(), 8u);
  ASSERT_EQ(telemetry->logical_push_reserved.load(), 0u);
  ASSERT_EQ(telemetry->logical_consumed.load(), 5u);
  ASSERT_EQ(telemetry->logical_unpushed_discarded.load(), 2u);
  ASSERT_EQ(telemetry->logical_queued_discarded.load(), 3u);
  ASSERT_EQ(telemetry->selected.load(), telemetry->consumed.load() +
                                                    telemetry->unpushed_discarded.load() +
                                                    telemetry->queued_discarded.load());
  ASSERT_EQ(telemetry->logical_selected.load(), telemetry->logical_consumed.load() +
                                                    telemetry->logical_unpushed_discarded.load() +
                                                    telemetry->logical_queued_discarded.load());
}

TEST(ExtMessagePoolScheduler, NativeQueueTracksPhysicalAndLogicalWorkSeparately) {
  auto telemetry = std::make_shared<ExtMsgQueueTelemetry>();
  ExtMsgQueueState state;
  state.attach_telemetry(telemetry);

  // Two BOCs represent four logical nonce/candidate slots: one three-output
  // run and one scalar NTFX. The queue-facing accounting must retain both
  // views without changing existing physical backpressure semantics.
  state.record_selected(2, 4);
  state.record_push_started(2, 4);
  state.record_push_completed(2, 2, 4, 4);
  state.record_consumed(2, 4);

  ASSERT_EQ(telemetry->selected.load(), 2u);
  ASSERT_EQ(telemetry->pushed.load(), 2u);
  ASSERT_EQ(telemetry->consumed.load(), 2u);
  ASSERT_EQ(telemetry->logical_selected.load(), 4u);
  ASSERT_EQ(telemetry->logical_pushed.load(), 4u);
  ASSERT_EQ(telemetry->logical_consumed.load(), 4u);
  ASSERT_EQ(state.native_selected_ahead(), 0u);
  ASSERT_EQ(state.native_logical_selected_ahead(), 0u);
}

}  // namespace ton::validator
