#include <tuple>

#include "td/utils/tests.h"
#include "td/actor/TestScheduler.h"
#include "validator/consensus/manager-facade.h"
#include "validator/impl/ext-message-pool.hpp"
#include "validator/impl/shard.hpp"

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
    td::optional<NativeAddress> cursor;
    td::uint64 scanned{0};
    td::uint64 selected{0};
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
    td::uint64 builds{0};
    td::uint64 source_scans{0};
    td::uint64 source_refreshes{0};
    td::uint64 source_probes{0};
    td::uint64 runs{0};
  };

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
                              td::uint64 fee = 0) {
    auto hash = make_bits(static_cast<td::uint32>(nonce + 1), static_cast<unsigned>(source.second.as_array()[0] + 64));
    auto message = td::make_ref<FakeExtMessage>(source.second, hash);
    auto mempool_message = std::make_shared<ExtMessagePool::MempoolMsg>(message);
    mempool_message->native_nonce = nonce;
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
    reservation.amount = amount;
    reservation.fee = fee;
    reservation.valid_until = std::numeric_limits<td::uint32>::max();
    reservation.account_revision = pool.native_nonce_watermarks_[source].revision;
    reservation.committed = committed;
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
    }
    result.scanned = selected.counters.scanned;
    result.selected = selected.counters.selected;
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
                                            std::size_t transport_message_capacity = 500) {
    auto callback = std::make_unique<ExtMsgCallback>();
    callback->shard = {basechainId, shardIdAll};
    callback->queue_capacity = queue_capacity;
    callback->transport_message_capacity = transport_message_capacity;
    callback->timeout = td::Timestamp::in(60.0);
    callback->native_streaming = true;
    auto installed = std::make_shared<ExtMessagePool::InstalledCallback>(std::move(callback));
    // Model the installed callback's serialized pump already waiting. This
    // keeps the unit test actor-free while ensuring the post-commit wake appends
    // work to the existing callback instead of creating another ingress event.
    installed->pump_active = true;
    pool.callbacks_.push_back(std::move(installed));
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
                                       .native_transfer = transfer};
    return pool.finalize_checked_message(std::move(result), priority, true, td::Timestamp::never()).is_ok();
  }

  static bool reservation_committed(const ExtMessagePool &pool, NativeAddress source, td::uint64 nonce) {
    return pool.native_accounts_.at(source).messages.at(nonce).committed;
  }

  static td::uint64 reservation_revision(const ExtMessagePool &pool, NativeAddress source, td::uint64 nonce) {
    return pool.native_accounts_.at(source).messages.at(nonce).account_revision;
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

  static std::size_t fill_once(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    auto callback = pool.callbacks_.front();
    pool.begin_callback_epoch(callback);
    return pool.fill_callback_native(callback, false);
  }

  static std::size_t prefill_native_transport(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    auto callback = pool.callbacks_.front();
    pool.begin_callback_epoch(callback);
    return pool.prefill_callback_native(callback, true);
  }

  static td::uint64 callback_selected_ahead(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.callbacks_.front()->callback->queue_state->native_selected_ahead();
  }

  static std::size_t callback_native_transport_selected_limit(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.native_transport_selected_limit(*pool.callbacks_.front());
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
    auto callback = pool.callbacks_.front();
    pool.begin_callback_epoch(callback);
    return pool.fill_callback_native(callback, false, &sources);
  }

  static SchedulerStats scheduler_stats(const ExtMessagePool &pool) {
    const auto &stats = pool.native_queue_counters_;
    return SchedulerStats{.selected = stats.selected,
                          .builds = stats.scheduler_builds,
                          .source_scans = stats.source_scans,
                          .source_refreshes = stats.source_refreshes,
                          .source_probes = stats.source_probes,
                          .runs = stats.runs};
  }

  static std::size_t callback_pending(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.callbacks_.front()->pending_native.size() + pool.callbacks_.front()->pending_generic.size();
  }

  static std::size_t wake_sources(ExtMessagePool &pool, const std::set<NativeAddress> &sources,
                                  bool preserve_valid_ready_head = false) {
    return pool.wake_native_callbacks(&sources, preserve_valid_ready_head);
  }

  static std::size_t dirty_sources(const ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
    return pool.callbacks_.front()->native_dirty_sources.size();
  }

  static std::size_t resume_native_pump_refill(ExtMessagePool &pool) {
    CHECK(pool.callbacks_.size() == 1);
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
};

static_assert(ExtMessagePoolTestAccess::max_native_queue_limit() == 65'536);
static_assert(ExtMessagePoolTestAccess::max_native_queue_limit() == block::NativeTransferBatch::max_entries);
static_assert(ExtMessagePoolTestAccess::native_delivery_chunk() == 512);

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
            transport_window + ExtMessagePoolTestAccess::native_delivery_chunk());
  ASSERT_TRUE(ExtMessagePoolTestAccess::callback_native_transport_has_refill_credit(pool));

  // Once the producer stages one additional native fragment while the window
  // is full, it has no credit to append another snapshot-sized backlog.
  ExtMessagePoolTestAccess::record_callback_selected(pool, ExtMessagePoolTestAccess::native_delivery_chunk());
  ASSERT_TRUE(!ExtMessagePoolTestAccess::callback_native_transport_has_refill_credit(pool));
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

  // Reserve the complete pump batch before it blocks. Cancellation initially
  // classifies the reservation as queue-owned; exact partial completion moves
  // only the failed suffix back to unpushed-discarded.
  state.record_push_started(4);
  ASSERT_EQ(telemetry->push_reserved.load(), 4u);
  ASSERT_EQ(telemetry->unpushed_discarded.load(), 0u);
  ASSERT_EQ(telemetry->queued_discarded.load(), 8u);
  state.record_push_completed(4, 2);
  state.record_consumed(3);
  ASSERT_EQ(telemetry->selected.load(), 10u);
  ASSERT_EQ(telemetry->pushed.load(), 8u);
  ASSERT_EQ(telemetry->push_reserved.load(), 0u);
  ASSERT_EQ(telemetry->consumed.load(), 5u);
  ASSERT_EQ(telemetry->unpushed_discarded.load(), 2u);
  ASSERT_EQ(telemetry->queued_discarded.load(), 3u);
  ASSERT_EQ(telemetry->selected.load(), telemetry->consumed.load() +
                                                    telemetry->unpushed_discarded.load() +
                                                    telemetry->queued_discarded.load());
}

}  // namespace ton::validator
