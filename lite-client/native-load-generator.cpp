/*
    High-rate native-transfer load generator for a private TON sidechain.

    The generator is deliberately a standalone process/container.  Workers own
    disjoint wallet ranges, signer actors and persistent ADNL/TCP connections.
    A coordinator aggregates metrics and follows proof-anchored basechain
    blocks so admission pressure is bounded by canonical progress.
*/
#include "auto/tl/lite_api.hpp"
#include "auto/tl/ton_api_json.h"
#include "block/block-auto.h"
#include "block/block.h"
#include "block/check-proof.h"
#include "block/mc-config.h"
#include "block/transaction.h"
#include "common/checksum.h"
#include "crypto/Ed25519.h"
#include "td/actor/actor.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/signals.h"
#include "tl-utils/lite-utils.hpp"
#include "ton/lite-tl.hpp"
#include "vm/boc.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"

#include "ext-client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <csignal>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;
std::atomic<int> process_exit_code{0};
constexpr td::uint64 max_native_nonce_diff = 4096;

constexpr bool nonce_in_half_open_cohort(td::uint64 nonce, td::uint64 begin,
                                         td::uint64 end) {
  return nonce >= begin && nonce < end;
}

constexpr bool canonical_cohorts_complete(td::uint64 measured_anchored,
                                          td::uint64 measured_offered,
                                          td::uint64 total_anchored,
                                          td::uint64 total_offered) {
  return measured_anchored >= measured_offered && total_anchored >= total_offered;
}

// Regression guards for two subtle benchmark rules: extending the half-open
// measured range makes an in-window proof count immediately, and completing
// only the measured cohort must never declare the full run drained.
static_assert(!nonce_in_half_open_cohort(7, 5, 7));
static_assert(nonce_in_half_open_cohort(7, 5, 8));
static_assert(!canonical_cohorts_complete(10, 10, 11, 12));
static_assert(canonical_cohorts_complete(10, 10, 12, 12));

void request_stop(int) {
  stop_requested = 1;
}

struct Options {
  std::string global_config{"/usr/share/data/global.config.json"};
  std::string wallet_dir{"/wallets"};
  td::uint32 source_offset{0};
  td::uint32 sources{1000};
  td::uint32 connections{4};
  td::uint32 signers{4};
  td::uint32 workers{1};
  td::uint32 max_inflight{8192};
  td::uint32 submit_batch_size{1};
  td::uint64 max_canonical_backlog{262144};
  td::uint32 max_source_canonical_backlog{64};
  td::uint32 duration_seconds{600};
  td::uint32 ramp_seconds{0};
  td::uint32 warmup_seconds{0};
  td::uint32 drain_timeout_seconds{30};
  td::uint32 valid_for_seconds{120};
  td::uint32 max_retries{3};
  td::uint32 retry_backoff_ms{10};
  td::uint32 finality_sample_sources{256};
  td::uint64 amount{1};
  td::uint64 fee{0};
  td::uint64 start_nonce{0};
  ton::Bits256 chain_domain{};
  double target_tps{0.0};
  double query_timeout{10.0};
  double report_interval{1.0};
  double finality_poll_seconds{10.0};
  double canonical_poll_seconds{0.25};
  double repair_cooldown_seconds{10.0};
  double adaptive_initial_rtt_seconds{1.0};
  bool auto_nonce{false};
  bool adaptive_inflight{false};
  bool canonical_block_follower{true};
};

td::Result<td::uint64> parse_nanograms(td::Slice value) {
  auto text = value.str();
  auto dot = text.find('.');
  auto whole_text = dot == std::string::npos ? text : text.substr(0, dot);
  auto fraction = dot == std::string::npos ? std::string{} : text.substr(dot + 1);
  if (whole_text.empty()) {
    whole_text = "0";
  }
  if (!std::all_of(whole_text.begin(), whole_text.end(), ::isdigit) ||
      !std::all_of(fraction.begin(), fraction.end(), ::isdigit) || fraction.size() > 9) {
    return td::Status::Error("amount must be a non-negative TON decimal with at most 9 fractional digits");
  }
  fraction.resize(9, '0');
  td::uint64 whole = td::to_integer<td::uint64>(whole_text);
  td::uint64 frac = fraction.empty() ? 0 : td::to_integer<td::uint64>(fraction);
  if (whole > (std::numeric_limits<td::uint64>::max() - frac) / 1000000000ULL) {
    return td::Status::Error("amount overflows uint64 nanograms");
  }
  return whole * 1000000000ULL + frac;
}

td::Result<ton::StdSmcAddress> read_address(td::CSlice filename) {
  TRY_RESULT(data, td::read_file(filename));
  if (data.size() != 32) {
    return td::Status::Error(PSLICE() << filename << " must contain exactly 32 raw public-key bytes");
  }
  ton::StdSmcAddress result;
  result.as_slice().copy_from(data.as_slice());
  return result;
}

td::BufferSlice envelope_query(td::BufferSlice query) {
  return ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_query>(std::move(query)), true);
}

td::Result<td::BufferSlice> unwrap_lite_result(td::Result<td::BufferSlice> result) {
  if (result.is_error()) {
    return result.move_as_error();
  }
  auto data = result.move_as_ok();
  auto lite_error = ton::fetch_tl_object<ton::lite_api::liteServer_error>(data.clone(), true);
  if (lite_error.is_ok()) {
    auto error = lite_error.move_as_ok();
    return td::Status::Error(error->code_, error->message_);
  }
  return std::move(data);
}

td::Result<td::uint64> parse_native_account_nonce(td::BufferSlice data, const ton::BlockIdExt& ref_mc,
                                                  const ton::StdSmcAddress& address) {
  TRY_RESULT(response, ton::fetch_tl_object<ton::lite_api::liteServer_accountState>(std::move(data), true));
  block::AccountState account_state;
  account_state.blk = ton::create_block_id(response->id_);
  account_state.shard_blk = ton::create_block_id(response->shardblk_);
  account_state.shard_proof = std::move(response->shard_proof_);
  account_state.proof = std::move(response->proof_);
  account_state.state = std::move(response->state_);
  TRY_RESULT(info, account_state.validate(ref_mc, block::StdAddress(ton::basechainId, address)));
  if (info.root.is_null()) {
    return td::Status::Error("native source account does not exist");
  }
  auto account_cs = vm::load_cell_slice(info.root);
  if (block::gen::t_Account.get_tag(account_cs) != block::gen::Account::account_native) {
    return td::Status::Error("source account is not a native balance-only account");
  }
  block::gen::Account::Record_account_native native;
  if (!tlb::unpack_cell(info.root, native)) {
    return td::Status::Error("cannot unpack native source account");
  }
  return native.nonce;
}

struct LatencyHistogram {
  static constexpr std::array<double, 20> upper_ms = {
      0.1,  0.25, 0.5,   1.0,   2.0,    5.0,    10.0,   20.0,   50.0,   100.0,
      200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0, 30000.0, 60000.0, 120000.0};
  std::array<td::uint64, upper_ms.size() + 1> buckets{};
  td::uint64 count{0};
  double max_ms{0.0};

  void observe_seconds(double seconds) {
    auto value = std::max(0.0, seconds * 1000.0);
    auto it = std::lower_bound(upper_ms.begin(), upper_ms.end(), value);
    ++buckets[static_cast<std::size_t>(it - upper_ms.begin())];
    ++count;
    max_ms = std::max(max_ms, value);
  }

  void merge(const LatencyHistogram& other) {
    for (std::size_t i = 0; i < buckets.size(); ++i) {
      buckets[i] += other.buckets[i];
    }
    count += other.count;
    max_ms = std::max(max_ms, other.max_ms);
  }

  double percentile(double quantile) const {
    if (!count) {
      return 0.0;
    }
    auto wanted = static_cast<td::uint64>(std::ceil(quantile * static_cast<double>(count)));
    wanted = std::max<td::uint64>(wanted, 1);
    td::uint64 current = 0;
    for (std::size_t i = 0; i < buckets.size(); ++i) {
      current += buckets[i];
      if (current >= wanted) {
        return i < upper_ms.size() ? upper_ms[i] : max_ms;
      }
    }
    return max_ms;
  }
};

struct WorkerStats {
  td::uint64 offered{0};
  td::uint64 steady_offered{0};
  td::uint64 submitted{0};
  td::uint64 steady_submitted{0};
  td::uint64 repair_submitted{0};
  td::uint64 sign_operations{0};
  td::uint64 sign_errors{0};
  td::uint64 wire_attempts{0};
  td::uint64 wire_queries{0};
  td::uint64 wire_batches{0};
  td::uint64 wire_batch_messages{0};
  td::uint64 max_wire_batch_size{0};
  td::uint64 retries{0};
  td::uint64 retry_exhausted{0};
  td::uint64 resigned{0};
  td::uint64 repair_offered{0};
  td::uint64 mempool_accepted{0};
  td::uint64 steady_mempool_accepted{0};
  td::uint64 repair_accepted{0};
  td::uint64 repeat_admission_successes{0};
  td::uint64 accepted_inferred{0};
  td::uint64 duplicate_nonce_conflicts{0};
  td::uint64 canonical_inferred_too_old{0};
  td::uint64 proof_observed_resolved{0};
  td::uint64 canonical_hash_matched{0};
  td::uint64 canonical_hash_conflicts{0};
  td::uint64 repair_suppressed{0};
  td::uint64 rejected{0};
  td::uint64 rejected_full{0};
  td::uint64 rejected_rate_limit{0};
  td::uint64 rejected_nonce{0};
  td::uint64 rejected_expired{0};
  td::uint64 rejected_balance{0};
  td::uint64 rejected_invalid{0};
  td::uint64 rejected_other{0};
  td::uint64 timeouts{0};
  td::uint64 transport_errors{0};
  td::uint64 server_errors{0};
  td::uint64 parse_errors{0};
  td::uint64 nonce_gaps{0};
  td::uint64 external_nonce_conflicts{0};
  td::uint64 anchor_scan_errors{0};
  td::uint64 anchored_at_end{0};
  td::uint64 anchored_after_drain{0};
  td::uint64 total_anchored_at_end{0};
  td::uint64 total_anchored_after_drain{0};
  td::uint64 canonical_backlog{0};
  td::uint64 canonical_backpressure_events{0};
  td::uint64 source_backpressure_stalls{0};
  td::uint64 inflight{0};
  td::uint64 signing{0};
  td::uint64 ready{0};
  td::uint64 retry_wait{0};
  td::uint64 active_tasks{0};
  td::uint64 active_sources{0};
  double congestion_window{0.0};
  double initial_congestion_window{0.0};
  double pacing_tokens{0.0};
  double measure_elapsed_seconds{0.0};
  double drain_to_anchor_seconds{0.0};
  double canonical_backpressure_seconds{0.0};
  bool steady_started{false};
  bool sending_done{false};
  bool end_snapshot_complete{false};
  bool drain_snapshot_complete{false};
  bool drain_timed_out{false};
  bool canonical_backpressure_paused{false};
  bool interrupted{false};
  LatencyHistogram signing_latency;
  LatencyHistogram request_latency;
  LatencyHistogram sampled_anchor_latency;
};

struct CanonicalTransferObservation {
  std::size_t wallet_idx{0};
  td::uint64 nonce{0};
  ton::Bits256 external_hash;
};

struct CanonicalFollowerStats {
  td::uint64 blocks{0};
  td::uint64 native_blocks{0};
  td::uint64 measured_native_blocks{0};
  td::uint64 native_transfers{0};
  td::uint64 measured_native_transfers{0};
  td::uint64 max_native_transfers_per_block{0};
  td::uint64 measured_max_native_transfers_per_block{0};
  td::uint64 errors{0};
  td::uint64 reorgs{0};
  td::uint64 lag_blocks{0};
  td::uint64 max_lag_blocks{0};
};

class NativeLoadCoordinator;

class NativeLoadWorker final : public td::actor::Actor {
 public:
  NativeLoadWorker(td::uint32 worker_id, Options options, std::vector<liteclient::LiteServerConfig> servers,
                   td::uint32 sample_sources, td::actor::ActorId<NativeLoadCoordinator> coordinator)
      : worker_id_(worker_id)
      , options_(std::move(options))
      , servers_(std::move(servers))
      , sample_sources_(sample_sources)
      , coordinator_(coordinator) {
  }

  void begin(double start_at) {
    if (failed_) {
      return;
    }
    start_at_ = start_at;
    last_token_at_ = start_at;
    next_sample_at_ = start_at + options_.finality_poll_seconds;
    next_publish_at_ = start_at;
    started_ = true;
    alarm_timestamp() = td::Timestamp::in(std::max(0.001, start_at_ - td::Time::now()));
  }

  void request_graceful_stop() {
    interrupted_ = true;
    begin_drain(td::Time::now());
  }

  void discover_startup_nonces(ton::BlockIdExt ref_mc);
  void observe_canonical_transfers(std::vector<CanonicalTransferObservation> observations);
  void canonical_checkpoint(bool may_capture_measure_end);
  void finalize_after_canonical_poll();

 private:
  struct SignedTransfer {
    td::BufferSlice boc;
    ton::Bits256 external_hash;
  };

  class Signer final : public td::actor::Actor {
   public:
    void sign(block::NativeTransfer transfer, ton::Bits256 chain_domain,
              std::shared_ptr<const td::Ed25519::PreparedPrivateKey> private_key,
              td::Promise<SignedTransfer> promise) {
      auto signature =
          td::Ed25519::PrivateKey::sign(*private_key, transfer.signing_payload(chain_domain));
      if (signature.is_error()) {
        promise.set_error(signature.move_as_error());
        return;
      }
      transfer.signature = signature.move_as_ok().as_slice().str();
      vm::CellBuilder builder;
      td::Ref<vm::Cell> root;
      if (!(transfer.store_external(builder) && builder.finalize_to(root))) {
        promise.set_error(td::Status::Error("failed to serialize native transfer"));
        return;
      }
      auto external_hash = ton::Bits256{root->get_hash().bits()};
      auto boc = vm::std_boc_serialize(std::move(root));
      if (boc.is_error()) {
        promise.set_error(boc.move_as_error());
      } else {
        promise.set_value(SignedTransfer{boc.move_as_ok(), external_hash});
      }
    }
  };

  enum class TaskState { signing, ready, inflight, retry_wait, resolved };
  enum class TaskResolution { admitted, proof_observed };

  struct TransferTask {
    std::size_t wallet_idx{0};
    block::NativeTransfer transfer;
    td::BufferSlice boc;
    TaskState state{TaskState::signing};
    td::uint32 attempts{0};
    double first_issued_at{0.0};
    double sign_started_at{0.0};
    double last_sent_at{0.0};
    double retry_at{0.0};
    bool measured{false};
    bool repair{false};
    bool ever_submitted{false};
    bool admission_counted{false};
    bool retry_exhaustion_counted{false};
    bool resigned_after_expiry{false};
  };

  struct AnchorSample {
    td::uint64 nonce{0};
    double issued_at{0.0};
  };

  struct Wallet {
    ton::StdSmcAddress source;
    ton::StdSmcAddress destination;
    std::shared_ptr<const td::Ed25519::PreparedPrivateKey> private_key;
    td::uint64 next_nonce{0};
    td::uint64 anchored_nonce{0};
    td::uint64 run_start_nonce{0};
    td::uint64 steady_start_nonce{0};
    td::uint64 steady_end_nonce{0};
    td::uint64 end_snapshot_nonce{0};
    td::uint64 canonical_total_matched{0};
    td::uint64 canonical_steady_matched{0};
    td::uint64 end_snapshot_total_matched{0};
    td::uint64 end_snapshot_steady_matched{0};
    bool sampled{false};
    bool disabled{false};
    bool available_queued{false};
    td::uint64 available_generation{0};
    td::uint64 last_repair_nonce{std::numeric_limits<td::uint64>::max()};
    double last_repair_at{0.0};
    // Multiple sequential nonces may be signing/in flight concurrently.  The
    // canonical backlog limits, rather than admission RTT, bound this window.
    std::map<td::uint64, std::shared_ptr<TransferTask>> tasks;
    // Admission is not canonical resolution. Retain the exact signed bytes so
    // drain repair can retry idempotently instead of creating a second hash
    // that collides with the still-pending nonce reservation.
    std::map<td::uint64, std::shared_ptr<TransferTask>> admitted_tasks;
    std::map<td::uint64, std::vector<ton::Bits256>> expected_hashes;
    std::deque<AnchorSample> samples;
  };

  struct AvailableWallet {
    std::size_t wallet_idx{0};
    td::uint64 generation{0};
  };

  struct ClientSlot {
    td::actor::ActorOwn<liteclient::ExtClient> actor;
    td::uint32 inflight{0};
    td::uint32 hard_limit{1};
    double cwnd{1.0};
    double last_decrease_at{0.0};
  };

  enum class ScanKind { none, startup, sample, end_boundary, drain };
  enum class ErrorOrigin { transport, server, parse };
  struct ScanItem {
    std::size_t wallet_idx{0};
    td::uint32 attempts{0};
  };

  td::uint32 worker_id_;
  Options options_;
  std::vector<liteclient::LiteServerConfig> servers_;
  td::uint32 sample_sources_{0};
  td::actor::ActorId<NativeLoadCoordinator> coordinator_;
  std::vector<Wallet> wallets_;
  std::vector<ClientSlot> clients_;
  std::vector<td::actor::ActorOwn<Signer>> signers_;
  std::deque<AvailableWallet> available_wallets_;
  std::deque<std::shared_ptr<TransferTask>> ready_tasks_;
  std::multimap<double, std::shared_ptr<TransferTask>> retry_tasks_;
  std::size_t signer_cursor_{0};
  std::size_t scan_client_cursor_{0};
  td::uint64 signing_{0};
  td::uint64 inflight_{0};
  td::uint64 active_tasks_{0};
  td::uint64 canonical_backlog_{0};
  WorkerStats stats_;
  bool started_{false};
  bool steady_started_{false};
  bool sending_done_{false};
  bool interrupted_{false};
  bool failed_{false};
  bool finished_{false};
  bool final_scan_grace_used_{false};
  double start_at_{0.0};
  double drain_started_at_{0.0};
  double drain_deadline_{0.0};
  double next_sample_at_{0.0};
  double next_publish_at_{0.0};
  double next_drain_scan_at_{0.0};
  double last_token_at_{0.0};
  double pacing_tokens_{0.0};
  ScanKind scan_kind_{ScanKind::none};
  ScanKind pending_scan_{ScanKind::none};
  ton::BlockIdExt scan_ref_mc_;
  std::deque<ScanItem> scan_items_;
  td::uint32 scan_inflight_{0};
  td::uint32 scan_failures_{0};
  bool end_snapshot_finished_{false};
  bool canonical_backpressure_paused_{false};
  double canonical_backpressure_started_at_{0.0};
  double canonical_backpressure_accumulated_{0.0};

  void start_up() override;
  td::Status initialize();
  void notify_ready();
  void fail(td::Status error);
  void alarm() override;
  void update_phase(double now);
  void update_tokens(double now);
  bool is_measure_phase(double now) const;
  bool can_issue(double now) const;
  bool source_backlog_full(const Wallet& wallet) const;
  bool task_is_active(const std::shared_ptr<TransferTask>& task) const;
  void update_backpressure_state(double now);
  void pump();
  td::optional<std::size_t> find_available_wallet();
  void enqueue_available_wallet(std::size_t wallet_idx);
  void invalidate_available_wallet(std::size_t wallet_idx);
  void create_transfer(std::size_t wallet_idx, td::uint64 nonce, bool measured, bool repair);
  void sign_task(std::shared_ptr<TransferTask> task, bool resign);
  void on_signed(std::shared_ptr<TransferTask> task, td::Result<SignedTransfer> message);
  td::optional<std::size_t> select_client() const;
  td::uint32 client_available_capacity(std::size_t client_idx) const;
  bool ready_for_first_submission(const std::shared_ptr<TransferTask>& task) const;
  std::shared_ptr<TransferTask> take_dispatchable_ready_task();
  void dispatch_ready();
  void send_task(std::shared_ptr<TransferTask> task, std::size_t client_idx);
  void send_batch(std::vector<std::shared_ptr<TransferTask>> tasks, std::size_t client_idx);
  void on_result(std::shared_ptr<TransferTask> task, std::size_t client_idx,
                 td::Result<td::BufferSlice> result);
  void on_batch_result(std::vector<std::shared_ptr<TransferTask>> tasks, std::size_t client_idx,
                       td::Result<td::BufferSlice> result);
  void handle_task_error(std::shared_ptr<TransferTask> task, std::size_t client_idx, td::Status error,
                         ErrorOrigin origin);
  void schedule_retry(std::shared_ptr<TransferTask> task, double delay_seconds);
  void accept_task(std::shared_ptr<TransferTask> task, TaskResolution resolution);
  void reject_task(std::shared_ptr<TransferTask> task, td::Slice reason);
  void disable_wallet_for_conflict(std::size_t wallet_idx, td::Slice reason);
  void begin_drain(double now);
  void maybe_finish();
  void finish();
  void refresh_stats();
  void publish_stats();

  void request_scan(ScanKind kind);
  void start_scan(ScanKind kind);
  void on_scan_masterchain(ScanKind kind, td::Result<td::BufferSlice> result);
  void pump_scan();
  void on_scan_account(ScanKind kind, ton::BlockIdExt ref_mc, ScanItem item,
                       td::Result<td::BufferSlice> result);
  void finish_scan();
  void apply_anchored_nonce(std::size_t wallet_idx, td::uint64 nonce, ScanKind kind, double observed_at);
  void repair_gaps();
  td::uint64 count_anchored(bool end_snapshot) const;
  td::uint64 count_total_anchored(bool end_snapshot) const;
};

class NativeLoadCoordinator final : public td::actor::Actor {
 public:
  explicit NativeLoadCoordinator(Options options) : options_(std::move(options)) {
  }

  void register_sources(td::uint32 worker_id, std::vector<ton::StdSmcAddress> sources) {
    if (worker_id >= source_registration_.size() || source_registration_[worker_id]) {
      return;
    }
    source_registration_[worker_id] = true;
    ++registered_source_workers_;
    for (std::size_t i = 0; i < sources.size(); ++i) {
      auto inserted = source_routes_.emplace(sources[i], SourceRoute{worker_id, i});
      if (!inserted.second) {
        worker_failed(worker_id, PSTRING() << "duplicate native source address " << sources[i].to_hex());
        return;
      }
    }
    if (options_.canonical_block_follower &&
        registered_source_workers_ == workers_.size() &&
        !startup_discovery_anchor_ready_ && !follower_query_active_) {
      // Establish the baseline only after every worker has loaded and
      // registered its wallet range.  This minimizes the block interval that
      // the mandatory pre-start ancestry catch-up must cover.
      request_follower_poll(true);
    }
    maybe_start_fixed_startup_discovery();
    maybe_begin();
  }

  void worker_ready(td::uint32 worker_id) {
    if (failed_ || worker_id >= worker_ready_.size() || worker_ready_[worker_id]) {
      return;
    }
    worker_ready_[worker_id] = true;
    ++ready_count_;
    maybe_begin();
  }

  void worker_stats(td::uint32 worker_id, WorkerStats stats) {
    if (worker_id < snapshots_.size()) {
      snapshots_[worker_id] = std::move(stats);
    }
  }

  void worker_done(td::uint32 worker_id, WorkerStats stats) {
    worker_stats(worker_id, std::move(stats));
    if (worker_id >= worker_done_.size() || worker_done_[worker_id]) {
      return;
    }
    worker_done_[worker_id] = true;
    ++done_count_;
    if (done_count_ == workers_.size()) {
      workers_complete_ = true;
      if (!options_.canonical_block_follower) {
        finish_process();
      } else if (!follower_query_active_) {
        final_follower_poll_ = true;
        request_follower_poll(false);
      }
    }
  }

  void worker_finalized(td::uint32 worker_id, WorkerStats stats) {
    worker_stats(worker_id, std::move(stats));
    if (worker_id >= worker_finalized_.size() || worker_finalized_[worker_id]) {
      return;
    }
    worker_finalized_[worker_id] = true;
    ++finalized_count_;
    if (finalized_count_ == workers_.size()) {
      finish_process();
    }
  }

  void worker_failed(td::uint32 worker_id, std::string error) {
    if (failed_) {
      return;
    }
    failed_ = true;
    process_exit_code.store(1);
    LOG(ERROR) << "native load worker " << worker_id << " failed: " << error;
    workers_.clear();
    td::actor::SchedulerContext::get().stop();
    stop();
  }

 private:
  struct SourceRoute {
    td::uint32 worker_id{0};
    std::size_t wallet_idx{0};
  };

  Options options_;
  std::vector<td::actor::ActorOwn<NativeLoadWorker>> workers_;
  std::vector<bool> worker_ready_;
  std::vector<bool> worker_done_;
  std::vector<bool> worker_finalized_;
  std::vector<bool> source_registration_;
  std::vector<WorkerStats> snapshots_;
  std::map<ton::StdSmcAddress, SourceRoute> source_routes_;
  WorkerStats previous_;
  CanonicalFollowerStats follower_stats_;
  CanonicalFollowerStats previous_follower_stats_;
  CanonicalFollowerStats follower_poll_delta_;
  td::actor::ActorOwn<liteclient::ExtClient> follower_client_;
  td::uint32 ready_count_{0};
  td::uint32 done_count_{0};
  td::uint32 finalized_count_{0};
  td::uint32 registered_source_workers_{0};
  bool started_{false};
  bool failed_{false};
  bool stop_sent_{false};
  bool follower_ready_{false};
  bool follower_start_catchup_requested_{false};
  bool follower_start_catchup_complete_{false};
  bool startup_discovery_anchor_ready_{false};
  bool startup_discovery_dispatched_{false};
  bool follower_query_active_{false};
  bool workers_complete_{false};
  bool final_follower_poll_{false};
  bool finalizing_workers_{false};
  double start_at_{0.0};
  double start_system_at_{0.0};
  double last_report_at_{0.0};
  double next_follower_poll_at_{0.0};
  double follower_poll_started_system_at_{0.0};
  ton::BlockIdExt followed_shard_block_;
  ton::BlockIdExt follower_target_block_;
  ton::BlockIdExt startup_discovery_mc_block_;
  std::vector<std::vector<CanonicalTransferObservation>> follower_observations_;
  std::map<ton::UnixTime, td::uint64> follower_measure_second_counts_;
  std::map<ton::UnixTime, td::uint64> follower_poll_measure_second_counts_;
  std::map<ton::UnixTime, td::uint64> follower_block_second_counts_;
  std::map<ton::UnixTime, td::uint64> follower_poll_block_second_counts_;
  td::uint64 canonical_backlog_sampled_peak_{0};
  double congestion_window_sampled_peak_{0.0};

  void maybe_begin();
  void maybe_start_fixed_startup_discovery();
  void request_follower_poll(bool baseline);
  void on_follower_masterchain(bool baseline, td::Result<td::BufferSlice> result);
  void on_follower_shards(bool baseline, ton::BlockIdExt mc_block, td::Result<td::BufferSlice> result);
  void request_follower_block(ton::BlockIdExt block_id);
  void on_follower_block(ton::BlockIdExt requested, td::Result<td::BufferSlice> result);
  void complete_follower_poll();
  void follower_error(td::Status error);
  void finalize_workers_after_follower();
  void finish_process();

  void start_up() override {
    auto config_data = td::read_file(options_.global_config);
    if (config_data.is_error()) {
      worker_failed(0, config_data.move_as_error().to_string());
      return;
    }
    auto config_buffer = config_data.move_as_ok();
    auto config_json = td::json_decode(config_buffer.as_slice());
    if (config_json.is_error()) {
      worker_failed(0, config_json.move_as_error().to_string());
      return;
    }
    auto config_value = config_json.move_as_ok();
    ton::ton_api::liteclient_config_global global;
    auto status = ton::ton_api::from_json(global, config_value.get_object());
    if (status.is_error()) {
      worker_failed(0, status.to_string());
      return;
    }
    if (!global.validator_ || !global.validator_->zero_state_) {
      worker_failed(0, "global config has no validator zero-state signature domain");
      return;
    }
    options_.chain_domain = global.validator_->zero_state_->root_hash_;
    auto parsed_servers = liteclient::LiteServerConfig::parse_global_config(global);
    if (parsed_servers.is_error()) {
      worker_failed(0, parsed_servers.move_as_error().to_string());
      return;
    }
    auto servers = parsed_servers.move_as_ok();
    if (servers.empty()) {
      worker_failed(0, "global config has no liteservers");
      return;
    }

    workers_.reserve(options_.workers);
    worker_ready_.resize(options_.workers);
    worker_done_.resize(options_.workers);
    worker_finalized_.resize(options_.workers);
    source_registration_.resize(options_.workers);
    snapshots_.resize(options_.workers);
    follower_observations_.resize(options_.workers);
    if (options_.canonical_block_follower) {
      follower_client_ = liteclient::ExtClient::create(servers, nullptr);
    } else {
      follower_ready_ = true;
    }
    td::uint32 source_cursor = 0;
    for (td::uint32 i = 0; i < options_.workers; ++i) {
      auto distribute = [i, count = options_.workers](td::uint32 total) {
        return total / count + (i < total % count ? 1u : 0u);
      };
      Options worker_options = options_;
      worker_options.source_offset = options_.source_offset + source_cursor;
      worker_options.sources = distribute(options_.sources);
      worker_options.connections = distribute(options_.connections);
      worker_options.signers = distribute(options_.signers);
      worker_options.max_inflight = distribute(options_.max_inflight);
      if (options_.max_canonical_backlog) {
        worker_options.max_canonical_backlog =
            options_.max_canonical_backlog / options_.workers +
            (i < options_.max_canonical_backlog % options_.workers ? 1 : 0);
      }
      worker_options.target_tps = options_.target_tps == 0.0
                                      ? 0.0
                                      : options_.target_tps * static_cast<double>(worker_options.sources) /
                                            static_cast<double>(options_.sources);
      auto worker_samples = distribute(std::min(options_.finality_sample_sources, options_.sources));
      source_cursor += worker_options.sources;
      workers_.push_back(td::actor::create_actor<NativeLoadWorker>(
          PSTRING() << "native-load-worker-" << i, i, std::move(worker_options), servers, worker_samples,
          actor_id(this)));
    }
    alarm_timestamp() = td::Timestamp::in(0.05);
  }

  void alarm() override {
    if (failed_) {
      return;
    }
    if (stop_requested && !stop_sent_) {
      stop_sent_ = true;
      if (!started_) {
        LOG(WARNING) << "native load generator interrupted during initialization";
        workers_.clear();
        td::actor::SchedulerContext::get().stop();
        stop();
        return;
      }
      for (auto& worker : workers_) {
        td::actor::send_closure(worker, &NativeLoadWorker::request_graceful_stop);
      }
    }
    if (!started_) {
      alarm_timestamp() = td::Timestamp::in(0.05);
      return;
    }
    auto now = td::Time::now();
    if (options_.canonical_block_follower && !follower_query_active_ && now >= next_follower_poll_at_) {
      request_follower_poll(false);
    }
    if (now - last_report_at_ >= options_.report_interval) {
      report(false);
      last_report_at_ = now;
    }
    if (done_count_ != workers_.size()) {
      alarm_timestamp() = td::Timestamp::in(std::min(0.1, options_.report_interval));
    }
  }

  WorkerStats aggregate() const {
    WorkerStats total;
    total.steady_started = true;
    total.sending_done = true;
    total.end_snapshot_complete = true;
    total.drain_snapshot_complete = true;
    for (const auto& value : snapshots_) {
#define ADD_FIELD(name) total.name += value.name
      ADD_FIELD(offered);
      ADD_FIELD(steady_offered);
      ADD_FIELD(submitted);
      ADD_FIELD(steady_submitted);
      ADD_FIELD(repair_submitted);
      ADD_FIELD(sign_operations);
      ADD_FIELD(sign_errors);
      ADD_FIELD(wire_attempts);
      ADD_FIELD(wire_queries);
      ADD_FIELD(wire_batches);
      ADD_FIELD(wire_batch_messages);
      ADD_FIELD(retries);
      ADD_FIELD(retry_exhausted);
      ADD_FIELD(resigned);
      ADD_FIELD(repair_offered);
      ADD_FIELD(mempool_accepted);
      ADD_FIELD(steady_mempool_accepted);
      ADD_FIELD(repair_accepted);
      ADD_FIELD(repeat_admission_successes);
      ADD_FIELD(accepted_inferred);
      ADD_FIELD(duplicate_nonce_conflicts);
      ADD_FIELD(canonical_inferred_too_old);
      ADD_FIELD(proof_observed_resolved);
      ADD_FIELD(canonical_hash_matched);
      ADD_FIELD(canonical_hash_conflicts);
      ADD_FIELD(repair_suppressed);
      ADD_FIELD(rejected);
      ADD_FIELD(rejected_full);
      ADD_FIELD(rejected_rate_limit);
      ADD_FIELD(rejected_nonce);
      ADD_FIELD(rejected_expired);
      ADD_FIELD(rejected_balance);
      ADD_FIELD(rejected_invalid);
      ADD_FIELD(rejected_other);
      ADD_FIELD(timeouts);
      ADD_FIELD(transport_errors);
      ADD_FIELD(server_errors);
      ADD_FIELD(parse_errors);
      ADD_FIELD(nonce_gaps);
      ADD_FIELD(external_nonce_conflicts);
      ADD_FIELD(anchor_scan_errors);
      ADD_FIELD(anchored_at_end);
      ADD_FIELD(anchored_after_drain);
      ADD_FIELD(total_anchored_at_end);
      ADD_FIELD(total_anchored_after_drain);
      ADD_FIELD(canonical_backlog);
      ADD_FIELD(canonical_backpressure_events);
      ADD_FIELD(source_backpressure_stalls);
      ADD_FIELD(inflight);
      ADD_FIELD(signing);
      ADD_FIELD(ready);
      ADD_FIELD(retry_wait);
      ADD_FIELD(active_tasks);
      ADD_FIELD(active_sources);
#undef ADD_FIELD
      total.congestion_window += value.congestion_window;
      total.initial_congestion_window += value.initial_congestion_window;
      total.max_wire_batch_size = std::max(total.max_wire_batch_size, value.max_wire_batch_size);
      total.pacing_tokens += value.pacing_tokens;
      total.measure_elapsed_seconds = std::max(total.measure_elapsed_seconds, value.measure_elapsed_seconds);
      total.drain_to_anchor_seconds = std::max(total.drain_to_anchor_seconds, value.drain_to_anchor_seconds);
      total.canonical_backpressure_seconds =
          std::max(total.canonical_backpressure_seconds, value.canonical_backpressure_seconds);
      total.steady_started = total.steady_started && value.steady_started;
      total.sending_done = total.sending_done && value.sending_done;
      total.end_snapshot_complete = total.end_snapshot_complete && value.end_snapshot_complete;
      total.drain_snapshot_complete = total.drain_snapshot_complete && value.drain_snapshot_complete;
      total.drain_timed_out = total.drain_timed_out || value.drain_timed_out;
      total.canonical_backpressure_paused =
          total.canonical_backpressure_paused || value.canonical_backpressure_paused;
      total.interrupted = total.interrupted || value.interrupted;
      total.signing_latency.merge(value.signing_latency);
      total.request_latency.merge(value.request_latency);
      total.sampled_anchor_latency.merge(value.sampled_anchor_latency);
    }
    return total;
  }

  std::string phase(double now, const WorkerStats& total) const {
    if (total.sending_done) {
      return "drain";
    }
    auto elapsed = std::max(0.0, now - start_at_);
    if (elapsed < options_.ramp_seconds) {
      return "ramp";
    }
    if (elapsed < options_.ramp_seconds + options_.warmup_seconds) {
      return "warmup";
    }
    return "measure";
  }

  void report(bool final) {
    auto now = td::Time::now();
    auto total = aggregate();
    canonical_backlog_sampled_peak_ =
        std::max(canonical_backlog_sampled_peak_, total.canonical_backlog);
    congestion_window_sampled_peak_ =
        std::max(congestion_window_sampled_peak_, total.congestion_window);
    td::uint64 canonical_measure_peak_1s = 0;
    for (const auto& [second, transfers] : follower_measure_second_counts_) {
      static_cast<void>(second);
      canonical_measure_peak_1s = std::max(canonical_measure_peak_1s, transfers);
    }
    td::uint64 canonical_blocks_peak_1s = 0;
    td::uint64 canonical_measure_blocks_peak_1s = 0;
    auto measure_begin_system = start_system_at_ + options_.ramp_seconds + options_.warmup_seconds;
    auto measure_end_system = measure_begin_system + options_.duration_seconds;
    for (const auto& [second, blocks] : follower_block_second_counts_) {
      canonical_blocks_peak_1s = std::max(canonical_blocks_peak_1s, blocks);
      if (second >= measure_begin_system && second < measure_end_system) {
        canonical_measure_blocks_peak_1s = std::max(canonical_measure_blocks_peak_1s, blocks);
      }
    }
    auto interval = std::max(0.000001, now - last_report_at_);
    auto rate = [interval](td::uint64 current, td::uint64 previous) {
      return static_cast<double>(current - previous) / interval;
    };
    bool canonical_result_valid = options_.canonical_block_follower && follower_stats_.errors == 0 &&
                                  follower_stats_.reorgs == 0 && total.canonical_hash_conflicts == 0 &&
                                  total.duplicate_nonce_conflicts == 0 && total.external_nonce_conflicts == 0;
    bool benchmark_result_valid = final && canonical_result_valid && !total.interrupted &&
                                  !total.drain_timed_out && total.end_snapshot_complete &&
                                  total.drain_snapshot_complete && follower_stats_.lag_blocks == 0 &&
                                  canonical_cohorts_complete(
                                      total.anchored_after_drain, total.steady_offered,
                                      total.total_anchored_after_drain, total.offered);
    std::cout << "{\"schema\":\"native-load-v2\",\"final\":" << (final ? "true" : "false")
              << ",\"phase\":\"" << (final ? "finished" : phase(now, total)) << "\",\"elapsed_s\":"
              << std::max(0.0, now - start_at_) << ",\"target_tps\":" << options_.target_tps
              << ",\"offered\":" << total.offered << ",\"steady_offered\":" << total.steady_offered
              << ",\"offered_tps\":" << rate(total.offered, previous_.offered)
              << ",\"submitted\":" << total.submitted << ",\"steady_submitted\":"
              << total.steady_submitted << ",\"repair_submitted\":" << total.repair_submitted
              << ",\"sign_operations\":" << total.sign_operations
              << ",\"sign_tps\":" << rate(total.sign_operations, previous_.sign_operations)
              << ",\"sign_errors\":" << total.sign_errors << ",\"wire_attempts\":" << total.wire_attempts
              << ",\"wire_tps\":" << rate(total.wire_attempts, previous_.wire_attempts)
              << ",\"wire_queries\":" << total.wire_queries << ",\"wire_query_tps\":"
              << rate(total.wire_queries, previous_.wire_queries) << ",\"wire_batches\":"
              << total.wire_batches << ",\"wire_batch_messages\":" << total.wire_batch_messages
              << ",\"wire_batch_avg_size\":"
              << static_cast<double>(total.wire_batch_messages) /
                     static_cast<double>(std::max<td::uint64>(1, total.wire_batches))
              << ",\"wire_batch_max_size\":" << total.max_wire_batch_size
              << ",\"retries\":" << total.retries << ",\"retry_exhausted\":" << total.retry_exhausted
              << ",\"resigned\":" << total.resigned << ",\"repair_offered\":" << total.repair_offered
              << ",\"mempool_accepted\":"
              << total.mempool_accepted << ",\"stored\":" << total.mempool_accepted
              << ",\"admitted\":" << total.mempool_accepted << ",\"mempool_accept_tps\":"
              << rate(total.mempool_accepted, previous_.mempool_accepted)
              << ",\"steady_mempool_accepted\":" << total.steady_mempool_accepted
              << ",\"repair_accepted\":" << total.repair_accepted
              << ",\"repeat_admission_successes\":" << total.repeat_admission_successes
              << ",\"accepted_inferred\":" << total.accepted_inferred
              << ",\"duplicate_nonce_conflicts\":" << total.duplicate_nonce_conflicts
              << ",\"canonical_inferred_too_old\":" << total.canonical_inferred_too_old
              << ",\"proof_observed_resolved\":" << total.proof_observed_resolved
              << ",\"canonical_hash_matched\":" << total.canonical_hash_matched
              << ",\"canonical_hash_conflicts\":" << total.canonical_hash_conflicts
              << ",\"repair_suppressed\":" << total.repair_suppressed
              << ",\"measure_elapsed_s\":" << total.measure_elapsed_seconds
              << ",\"steady_offered_avg_tps\":"
              << static_cast<double>(total.steady_offered) / std::max(0.000001, total.measure_elapsed_seconds)
              << ",\"steady_mempool_accept_avg_tps\":"
              << static_cast<double>(total.steady_mempool_accepted) /
                     std::max(0.000001, total.measure_elapsed_seconds)
              << ",\"rejected\":" << total.rejected
              << ",\"submission_errors_by_reason\":{\"full\":" << total.rejected_full << ",\"rate_limit\":"
              << total.rejected_rate_limit << ",\"nonce\":" << total.rejected_nonce << ",\"expired\":"
              << total.rejected_expired << ",\"balance\":" << total.rejected_balance << ",\"invalid\":"
              << total.rejected_invalid << ",\"other\":" << total.rejected_other << "}"
              << ",\"timeouts\":" << total.timeouts << ",\"transport_errors\":" << total.transport_errors
              << ",\"server_errors\":" << total.server_errors << ",\"parse_errors\":" << total.parse_errors
              << ",\"nonce_gaps\":" << total.nonce_gaps << ",\"external_nonce_conflicts\":"
              << total.external_nonce_conflicts << ",\"inflight\":" << total.inflight
              << ",\"canonical_backlog\":" << total.canonical_backlog
              << ",\"canonical_backlog_sampled_peak\":" << canonical_backlog_sampled_peak_
              << ",\"canonical_backpressure_paused\":"
              << (total.canonical_backpressure_paused ? "true" : "false")
              << ",\"canonical_backpressure_events\":" << total.canonical_backpressure_events
              << ",\"canonical_backpressure_s\":" << total.canonical_backpressure_seconds
              << ",\"source_backpressure_stalls\":" << total.source_backpressure_stalls
              << ",\"signing\":" << total.signing << ",\"ready\":" << total.ready
              << ",\"retry_wait\":" << total.retry_wait << ",\"active_tasks\":"
              << total.active_tasks << ",\"active_sources\":" << total.active_sources
              << ",\"interrupted\":" << (total.interrupted ? "true" : "false")
              << ",\"drain_timed_out\":" << (total.drain_timed_out ? "true" : "false")
              << ",\"congestion_window\":" << total.congestion_window
              << ",\"initial_congestion_window\":" << total.initial_congestion_window
              << ",\"congestion_window_sampled_peak\":" << congestion_window_sampled_peak_
              << ",\"pacing_tokens\":"
              << total.pacing_tokens << ",\"rtt_ms\":{\"p50\":" << total.request_latency.percentile(0.50)
              << ",\"p95\":" << total.request_latency.percentile(0.95) << ",\"p99\":"
              << total.request_latency.percentile(0.99) << ",\"max\":" << total.request_latency.max_ms
              << ",\"samples\":" << total.request_latency.count << "},\"sign_ms\":{\"p50\":"
              << total.signing_latency.percentile(0.50) << ",\"p95\":"
              << total.signing_latency.percentile(0.95) << ",\"p99\":"
              << total.signing_latency.percentile(0.99) << ",\"max\":" << total.signing_latency.max_ms
              << ",\"samples\":" << total.signing_latency.count << "},\"anchor_latency_sample_ms\":{\"p50\":"
              << total.sampled_anchor_latency.percentile(0.50) << ",\"p95\":"
              << total.sampled_anchor_latency.percentile(0.95) << ",\"p99\":"
              << total.sampled_anchor_latency.percentile(0.99) << ",\"max\":"
              << total.sampled_anchor_latency.max_ms << ",\"samples\":" << total.sampled_anchor_latency.count
              << ",\"poll_resolution_s\":" << options_.finality_poll_seconds << "},\"anchor_scan_errors\":"
              << total.anchor_scan_errors << ",\"canonical_chain_blocks\":" << follower_stats_.blocks
              << ",\"canonical_follower_block_discovery_rate\":"
              << rate(follower_stats_.blocks, previous_follower_stats_.blocks)
              << ",\"canonical_chain_native_blocks\":" << follower_stats_.native_blocks
              << ",\"canonical_chain_measure_native_blocks\":"
              << follower_stats_.measured_native_blocks
              << ",\"canonical_chain_native_transfers\":" << follower_stats_.native_transfers
              << ",\"canonical_chain_avg_native_transfers_per_block\":"
              << static_cast<double>(follower_stats_.native_transfers) /
                     static_cast<double>(std::max<td::uint64>(1, follower_stats_.native_blocks))
              << ",\"canonical_chain_measure_avg_native_transfers_per_block\":"
              << static_cast<double>(follower_stats_.measured_native_transfers) /
                     static_cast<double>(std::max<td::uint64>(1, follower_stats_.measured_native_blocks))
              << ",\"canonical_chain_max_native_transfers_per_block\":"
              << follower_stats_.max_native_transfers_per_block
              << ",\"canonical_chain_measure_max_native_transfers_per_block\":"
              << follower_stats_.measured_max_native_transfers_per_block
              << ",\"canonical_chain_max_blocks_per_gen_utime_second\":"
              << canonical_blocks_peak_1s
              << ",\"canonical_chain_measure_max_blocks_per_gen_utime_second\":"
              << canonical_measure_blocks_peak_1s
              << ",\"canonical_follower_discovery_tps\":"
              << rate(follower_stats_.native_transfers, previous_follower_stats_.native_transfers)
              << ",\"canonical_chain_measure_transfers\":"
              << follower_stats_.measured_native_transfers
              << ",\"canonical_chain_measure_avg_tps\":"
              << static_cast<double>(follower_stats_.measured_native_transfers) /
                     std::max(0.000001, total.measure_elapsed_seconds)
              << ",\"canonical_chain_measure_peak_1s_tps\":" << canonical_measure_peak_1s
              << ",\"canonical_follower_errors\":" << follower_stats_.errors
              << ",\"canonical_follower_reorgs\":" << follower_stats_.reorgs
              << ",\"canonical_follower_lag_blocks\":" << follower_stats_.lag_blocks
              << ",\"canonical_follower_max_lag_blocks\":" << follower_stats_.max_lag_blocks
              << ",\"canonical_follower_enabled\":"
              << (options_.canonical_block_follower ? "true" : "false")
              << ",\"canonical_result_valid\":" << (canonical_result_valid ? "true" : "false")
              << ",\"benchmark_result_valid\":"
              << (final ? (benchmark_result_valid ? "true" : "false") : "null")
              << ",\"canonical_at_measure_end\":";
    if (total.end_snapshot_complete) {
      std::cout << total.anchored_at_end << ",\"canonical_measured_offers_at_measure_end\":"
                << total.anchored_at_end
                << ",\"canonical_measured_offers_at_first_post_measure_checkpoint\":"
                << total.anchored_at_end << ",\"canonical_total_at_measure_end\":"
                << total.total_anchored_at_end << ",\"canonical_backlog_at_measure_end\":"
                << (total.steady_offered >= total.anchored_at_end ? total.steady_offered - total.anchored_at_end : 0)
                << ",\"canonical_measured_offer_cohort_observed_avg_tps\":"
                << static_cast<double>(total.anchored_at_end) /
                       std::max(0.000001, total.measure_elapsed_seconds);
    } else {
      std::cout << "null,\"canonical_measured_offers_at_measure_end\":null"
                   ",\"canonical_measured_offers_at_first_post_measure_checkpoint\":null"
                   ",\"canonical_total_at_measure_end\":null"
                   ",\"canonical_backlog_at_measure_end\":null"
                   ",\"canonical_measured_offer_cohort_observed_avg_tps\":null";
    }
    std::cout << ",\"canonical_after_drain\":";
    if (total.drain_snapshot_complete) {
      std::cout << total.anchored_after_drain << ",\"canonical_measured_offers_after_drain\":"
                << total.anchored_after_drain << ",\"canonical_total_after_drain\":"
                << total.total_anchored_after_drain << ",\"canonical_total_backlog_after_drain\":"
                << (total.offered >= total.total_anchored_after_drain
                        ? total.offered - total.total_anchored_after_drain
                        : 0)
                << ",\"canonical_backlog_after_drain\":"
                << (total.steady_offered >= total.anchored_after_drain
                        ? total.steady_offered - total.anchored_after_drain
                        : 0)
                << ",\"drain_to_anchor_s\":" << total.drain_to_anchor_seconds;
    } else {
      std::cout << "null,\"canonical_measured_offers_after_drain\":null"
                   ",\"canonical_total_after_drain\":null,\"canonical_total_backlog_after_drain\":null"
                   ",\"canonical_backlog_after_drain\":null,\"drain_to_anchor_s\":null";
    }
    std::cout << ",\"finalized\":null,\"finalized_semantics\":\"not_independently_observed\""
              << ",\"admission_semantics\":\"liteServer.sendMessage status=1; not block inclusion\""
              << ",\"benchmark_result_valid_semantics\":\"final proof-consistent run with complete measured and total cohorts, zero drain timeout, and follower caught up to the anchored shard tip\""
              << ",\"repeat_admission_successes_semantics\":\"status=1 responses for a nonce whose admission was already counted; excluded from mempool_accepted and TPS\""
              << ",\"duplicate_semantics\":\"different or concurrently unresolved message owns the nonce; conflict, never admission or canonical proof\""
              << ",\"accepted_inferred_semantics\":\"deprecated proof-resolution counter; do not use as TPS\""
              << ",\"canonical_inferred_semantics\":\"too-old response from node; not proof-observed\""
              << ",\"canonical_hash_match_semantics\":\"decoded canonical source, nonce, and external-cell hash exactly match a transfer signed by this run\""
              << ",\"canonical_chain_semantics\":\"verified basechain blocks referenced by ShardHashes proven against the configured liteserver masterchain tip\""
              << ",\"canonical_follower_discovery_tps_semantics\":\"observer catch-up throughput; not chain production TPS\""
              << ",\"canonical_chain_measure_peak_1s_tps_semantics\":\"maximum transfers assigned to one integer block gen_utime second inside the measurement window\""
              << ",\"canonical_chain_block_packing_semantics\":\"native transfer batch entries decoded from proof-checked canonical blocks; gen_utime peaks use one-second buckets\""
              << ",\"canonical_backlog_sampled_peak_semantics\":\"maximum summed latest-worker backlog gauge at report samples; not an exact instantaneous peak\""
              << ",\"congestion_window_sampled_peak_semantics\":\"maximum summed latest-worker AIMD window gauge at report samples\""
              << ",\"canonical_at_measure_end_semantics\":\"legacy cohort alias sampled by the first fully verified follower poll whose masterchain query began after offering ended\""
              << ",\"canonical_measured_offer_cohort_observed_avg_tps_semantics\":\"measured-offer cohort proven by the first post-measure checkpoint divided by offer-window duration; not chain-window production TPS\""
              << ",\"canonical_measured_offer_semantics\":\"measured-phase issued nonce cohort; distinct from chain progress during the measurement window\""
              << ",\"canonical_semantics\":\"proof_checked_against_configured_liteserver_tip_shard_history\"}"
              << std::endl;
    previous_ = total;
    previous_follower_stats_ = follower_stats_;
  }
};

void NativeLoadCoordinator::maybe_begin() {
  if (started_ || failed_ || ready_count_ != workers_.size() ||
      registered_source_workers_ != workers_.size() || !follower_ready_) {
    return;
  }
  CHECK(!options_.auto_nonce || !options_.canonical_block_follower ||
        (startup_discovery_dispatched_ && startup_discovery_anchor_ready_ &&
         startup_discovery_mc_block_.is_valid_full()));
  if (options_.canonical_block_follower && !follower_start_catchup_complete_) {
    if (follower_start_catchup_requested_) {
      return;
    }
    // Account discovery was bound to the original proof-checked baseline.
    // Follow forward from that exact shard tip instead of replacing it with a
    // newer baseline, otherwise blocks committed during discovery disappear
    // from both the account snapshot and follower history.
    follower_start_catchup_requested_ = true;
    follower_ready_ = false;
    request_follower_poll(false);
    return;
  }
  start_at_ = td::Time::now() + 0.2;
  start_system_at_ = td::Clocks::system() + 0.2;
  started_ = true;
  for (auto& worker : workers_) {
    td::actor::send_closure(worker, &NativeLoadWorker::begin, start_at_);
  }
  LOG(WARNING) << "native load generator ready: workers=" << workers_.size()
               << " sources=" << options_.sources << " connections=" << options_.connections
               << " signers=" << options_.signers << " max_inflight=" << options_.max_inflight
               << " target_tps=" << options_.target_tps << " ramp=" << options_.ramp_seconds
               << "s warmup=" << options_.warmup_seconds << "s duration=" << options_.duration_seconds
               << "s drain_timeout=" << options_.drain_timeout_seconds << "s canonical_backlog="
               << options_.max_canonical_backlog << " source_backlog="
               << options_.max_source_canonical_backlog << " canonical_follow="
               << options_.canonical_block_follower << " adaptive_initial_rtt_s="
               << options_.adaptive_initial_rtt_seconds;
  last_report_at_ = start_at_;
  next_follower_poll_at_ = start_at_;
  alarm_timestamp() = td::Timestamp::in(std::max(0.001, start_at_ - td::Time::now()));
}

void NativeLoadCoordinator::maybe_start_fixed_startup_discovery() {
  if (!options_.auto_nonce || !options_.canonical_block_follower ||
      !startup_discovery_anchor_ready_ || startup_discovery_dispatched_ ||
      registered_source_workers_ != workers_.size()) {
    return;
  }
  CHECK(startup_discovery_mc_block_.is_valid_full());
  startup_discovery_dispatched_ = true;
  LOG(WARNING) << "discovering all native source nonces at follower baseline "
               << startup_discovery_mc_block_.to_str();
  for (auto& worker : workers_) {
    td::actor::send_closure(worker, &NativeLoadWorker::discover_startup_nonces,
                            startup_discovery_mc_block_);
  }
}

void NativeLoadCoordinator::request_follower_poll(bool baseline) {
  if (!options_.canonical_block_follower || follower_query_active_ || follower_client_.empty()) {
    return;
  }
  follower_query_active_ = true;
  follower_poll_started_system_at_ = td::Clocks::system();
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfo>(), true);
  auto promise = td::PromiseCreator::lambda(
      [self = actor_id(this), baseline](td::Result<td::BufferSlice> result) mutable {
        td::actor::send_closure(self, &NativeLoadCoordinator::on_follower_masterchain, baseline,
                                std::move(result));
      });
  td::actor::send_closure(follower_client_, &liteclient::ExtClient::send_query, "native-load-follow-mc",
                          envelope_query(std::move(query)), td::Timestamp::in(options_.query_timeout),
                          std::move(promise));
}

void NativeLoadCoordinator::on_follower_masterchain(bool baseline, td::Result<td::BufferSlice> result) {
  auto data = unwrap_lite_result(std::move(result));
  if (data.is_error()) {
    follower_error(data.move_as_error_prefix("cannot obtain masterchain anchor: "));
    return;
  }
  auto parsed = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfo>(data.move_as_ok(), true);
  if (parsed.is_error()) {
    follower_error(parsed.move_as_error_prefix("cannot parse masterchain anchor: "));
    return;
  }
  auto mc_block = ton::create_block_id(parsed.move_as_ok()->last_);
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(
          ton::create_tl_lite_block_id(mc_block)),
      true);
  auto promise = td::PromiseCreator::lambda(
      [self = actor_id(this), baseline, mc_block](td::Result<td::BufferSlice> shard_result) mutable {
        td::actor::send_closure(self, &NativeLoadCoordinator::on_follower_shards, baseline, mc_block,
                                std::move(shard_result));
      });
  td::actor::send_closure(follower_client_, &liteclient::ExtClient::send_query, "native-load-follow-shards",
                          envelope_query(std::move(query)), td::Timestamp::in(options_.query_timeout),
                          std::move(promise));
}

void NativeLoadCoordinator::on_follower_shards(bool baseline, ton::BlockIdExt mc_block,
                                                td::Result<td::BufferSlice> result) {
  auto data = unwrap_lite_result(std::move(result));
  if (data.is_error()) {
    follower_error(data.move_as_error_prefix("cannot obtain anchored shard configuration: "));
    return;
  }
  auto parsed = ton::fetch_tl_object<ton::lite_api::liteServer_allShardsInfo>(data.move_as_ok(), true);
  if (parsed.is_error()) {
    follower_error(parsed.move_as_error_prefix("cannot parse anchored shard configuration: "));
    return;
  }
  auto response = parsed.move_as_ok();
  if (ton::create_block_id(response->id_) != mc_block) {
    follower_error(td::Status::Error("getAllShardsInfo returned a different masterchain block"));
    return;
  }
  auto proof_root = vm::std_boc_deserialize(response->proof_.clone());
  if (proof_root.is_error()) {
    follower_error(proof_root.move_as_error_prefix("cannot deserialize shard configuration proof: "));
    return;
  }
  auto virtual_root = vm::MerkleProof::virtualize(proof_root.move_as_ok());
  if (virtual_root.is_error()) {
    follower_error(virtual_root.move_as_error_prefix("cannot virtualize shard configuration proof: "));
    return;
  }
  auto mc_root = virtual_root.move_as_ok();
  // getAllShardsInfo deliberately returns a *partial* block Merkle proof. The
  // server opens Block.extra -> McBlockExtra.shard_hashes and prunes unrelated
  // header branches (prev refs, state update, etc.). check_block_header_proof
  // traverses those unrelated branches and therefore rejects a valid response
  // with "prunned branch". MerkleProof::virtualize has already validated the
  // proof wrapper; bind its virtual root directly to the requested MC root and
  // below parse only the opened, response-relevant path.
  auto proven_root_hash = ton::RootHash{mc_root->get_hash().bits()};
  if (proven_root_hash != mc_block.root_hash) {
    follower_error(td::Status::Error(
        PSLICE() << "getAllShardsInfo proof root hash mismatch: proven="
                 << proven_root_hash.to_hex() << " expected=" << mc_block.root_hash.to_hex()));
    return;
  }
  block::gen::Block::Record block_record;
  block::gen::BlockExtra::Record block_extra;
  block::gen::McBlockExtra::Record mc_extra;
  if (!tlb::unpack_cell(mc_root, block_record) || !tlb::unpack_cell(block_record.extra, block_extra) ||
      !block_extra.custom->have_refs() ||
      !tlb::unpack_cell(block_extra.custom->prefetch_ref(), mc_extra)) {
    follower_error(td::Status::Error("cannot unpack shard configuration from masterchain proof"));
    return;
  }
  auto shard_data = vm::std_boc_deserialize(response->data_.clone());
  if (shard_data.is_error()) {
    follower_error(shard_data.move_as_error_prefix("cannot deserialize ShardHashes data: "));
    return;
  }
  auto shard_data_root = shard_data.move_as_ok();
  auto shard_data_cs = vm::load_cell_slice(shard_data_root);
  if (!mc_extra.shard_hashes->contents_equal(shard_data_cs)) {
    follower_error(td::Status::Error("ShardHashes data does not match the masterchain proof"));
    return;
  }
  block::ShardConfig shard_config;
  if (!shard_config.unpack(vm::load_cell_slice_ref(shard_data_root))) {
    follower_error(td::Status::Error("cannot unpack proof-checked ShardHashes"));
    return;
  }
  auto shard = shard_config.get_shard_hash(ton::ShardIdFull{ton::basechainId, ton::shardIdAll});
  if (shard.is_null()) {
    follower_error(td::Status::Error("masterchain has no unsplit basechain shard"));
    return;
  }
  auto top = shard->top_block_id();
  if (baseline || !followed_shard_block_.is_valid_full()) {
    followed_shard_block_ = top;
    startup_discovery_mc_block_ = mc_block;
    startup_discovery_anchor_ready_ = true;
    follower_stats_.lag_blocks = 0;
    follower_query_active_ = false;
    follower_ready_ = true;
    next_follower_poll_at_ = td::Time::now() + options_.canonical_poll_seconds;
    maybe_start_fixed_startup_discovery();
    maybe_begin();
    return;
  }
  if (top == followed_shard_block_) {
    follower_stats_.lag_blocks = 0;
    complete_follower_poll();
    return;
  }
  if (top.shard_full() != followed_shard_block_.shard_full() ||
      top.seqno() <= followed_shard_block_.seqno()) {
    ++follower_stats_.reorgs;
    follower_error(td::Status::Error(PSLICE() << "anchored basechain tip does not extend followed tip: old="
                                               << followed_shard_block_.to_str() << " new=" << top.to_str()));
    return;
  }
  follower_target_block_ = top;
  follower_stats_.lag_blocks = top.seqno() - followed_shard_block_.seqno();
  follower_stats_.max_lag_blocks =
      std::max(follower_stats_.max_lag_blocks, follower_stats_.lag_blocks);
  follower_poll_delta_ = {};
  follower_poll_measure_second_counts_.clear();
  follower_poll_block_second_counts_.clear();
  for (auto& observations : follower_observations_) {
    observations.clear();
  }
  request_follower_block(top);
}

void NativeLoadCoordinator::request_follower_block(ton::BlockIdExt block_id) {
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getBlock>(ton::create_tl_lite_block_id(block_id)), true);
  auto promise = td::PromiseCreator::lambda(
      [self = actor_id(this), block_id](td::Result<td::BufferSlice> result) mutable {
        td::actor::send_closure(self, &NativeLoadCoordinator::on_follower_block, block_id, std::move(result));
      });
  td::actor::send_closure(follower_client_, &liteclient::ExtClient::send_query, "native-load-follow-block",
                          envelope_query(std::move(query)), td::Timestamp::in(options_.query_timeout),
                          std::move(promise));
}

void NativeLoadCoordinator::on_follower_block(ton::BlockIdExt requested,
                                               td::Result<td::BufferSlice> result) {
  auto data = unwrap_lite_result(std::move(result));
  if (data.is_error()) {
    follower_error(data.move_as_error_prefix("cannot obtain anchored basechain block: "));
    return;
  }
  auto parsed = ton::fetch_tl_object<ton::lite_api::liteServer_blockData>(data.move_as_ok(), true);
  if (parsed.is_error()) {
    follower_error(parsed.move_as_error_prefix("cannot parse anchored basechain block: "));
    return;
  }
  auto response = parsed.move_as_ok();
  auto response_id = ton::create_block_id(response->id_);
  if (response_id != requested || td::sha256_bits256(response->data_.as_slice()) != requested.file_hash) {
    follower_error(td::Status::Error("anchored basechain block id or file hash mismatch"));
    return;
  }
  auto root_result = vm::std_boc_deserialize(response->data_.clone());
  if (root_result.is_error()) {
    follower_error(root_result.move_as_error_prefix("cannot deserialize anchored basechain block: "));
    return;
  }
  auto root = root_result.move_as_ok();
  auto header_status = block::check_block_header_proof(root, requested);
  if (header_status.is_error()) {
    follower_error(header_status.move_as_error_prefix("invalid anchored basechain block: "));
    return;
  }
  block::gen::Block::Record block_record;
  block::gen::BlockInfo::Record block_info;
  block::gen::BlockExtra::Record block_extra;
  if (!tlb::unpack_cell(root, block_record) || !tlb::unpack_cell(block_record.info, block_info) ||
      !block_info.not_master || !tlb::unpack_cell(block_record.extra, block_extra)) {
    follower_error(td::Status::Error("cannot unpack anchored basechain block"));
    return;
  }
  ++follower_poll_delta_.blocks;
  ++follower_poll_block_second_counts_[block_info.gen_utime];
  if (block_extra.custom->have_refs()) {
    auto batch = block::NativeTransferBatch::unpack(block_extra.custom->prefetch_ref());
    if (batch.is_error()) {
      follower_error(batch.move_as_error_prefix("cannot unpack canonical native transfer batch: "));
      return;
    }
    auto native_batch = batch.move_as_ok();
    ++follower_poll_delta_.native_blocks;
    follower_poll_delta_.max_native_transfers_per_block =
        std::max<td::uint64>(follower_poll_delta_.max_native_transfers_per_block,
                             native_batch.entries.size());
    follower_poll_delta_.native_transfers += native_batch.entries.size();
    auto measure_begin = start_system_at_ + options_.ramp_seconds + options_.warmup_seconds;
    auto measure_end = measure_begin + options_.duration_seconds;
    if (block_info.gen_utime >= measure_begin && block_info.gen_utime < measure_end) {
      ++follower_poll_delta_.measured_native_blocks;
      follower_poll_delta_.measured_native_transfers += native_batch.entries.size();
      follower_poll_delta_.measured_max_native_transfers_per_block =
          std::max<td::uint64>(follower_poll_delta_.measured_max_native_transfers_per_block,
                               native_batch.entries.size());
      follower_poll_measure_second_counts_[block_info.gen_utime] += native_batch.entries.size();
    }
    for (const auto& entry : native_batch.entries) {
      auto route = source_routes_.find(entry.transfer.src);
      if (route == source_routes_.end() || entry.transfer.nonce == std::numeric_limits<td::uint64>::max()) {
        continue;
      }
      auto external_hash = entry.transfer.external_hash();
      if (external_hash.is_error()) {
        follower_error(external_hash.move_as_error_prefix(
            "cannot hash decoded canonical native transfer: "));
        return;
      }
      follower_observations_[route->second.worker_id].push_back(
          CanonicalTransferObservation{route->second.wallet_idx, entry.transfer.nonce,
                                       external_hash.move_as_ok()});
    }
  }
  std::vector<ton::BlockIdExt> previous;
  ton::BlockIdExt referenced_mc;
  bool after_split = false;
  auto prev_status = block::unpack_block_prev_blk_try(root, requested, previous, referenced_mc, after_split);
  if (prev_status.is_error() || previous.size() != 1) {
    follower_error(prev_status.is_error()
                       ? prev_status.move_as_error_prefix("cannot follow anchored basechain ancestry: ")
                       : td::Status::Error("canonical follower supports exactly one unsplit basechain shard"));
    return;
  }
  if (previous[0] == followed_shard_block_) {
    followed_shard_block_ = follower_target_block_;
    complete_follower_poll();
    return;
  }
  if (previous[0].seqno() >= requested.seqno() || previous[0].seqno() <= followed_shard_block_.seqno()) {
    ++follower_stats_.reorgs;
    follower_error(td::Status::Error("anchored basechain ancestry does not reach the followed tip"));
    return;
  }
  request_follower_block(previous[0]);
}

void NativeLoadCoordinator::complete_follower_poll() {
  follower_stats_.blocks += follower_poll_delta_.blocks;
  follower_stats_.native_blocks += follower_poll_delta_.native_blocks;
  follower_stats_.measured_native_blocks += follower_poll_delta_.measured_native_blocks;
  follower_stats_.native_transfers += follower_poll_delta_.native_transfers;
  follower_stats_.measured_native_transfers += follower_poll_delta_.measured_native_transfers;
  follower_stats_.max_native_transfers_per_block =
      std::max(follower_stats_.max_native_transfers_per_block,
               follower_poll_delta_.max_native_transfers_per_block);
  follower_stats_.measured_max_native_transfers_per_block =
      std::max(follower_stats_.measured_max_native_transfers_per_block,
               follower_poll_delta_.measured_max_native_transfers_per_block);
  for (const auto& [second, transfers] : follower_poll_measure_second_counts_) {
    follower_measure_second_counts_[second] += transfers;
  }
  for (const auto& [second, blocks] : follower_poll_block_second_counts_) {
    follower_block_second_counts_[second] += blocks;
  }
  follower_stats_.lag_blocks = 0;
  follower_poll_delta_ = {};
  follower_poll_measure_second_counts_.clear();
  follower_poll_block_second_counts_.clear();
  for (std::size_t worker_id = 0; worker_id < follower_observations_.size(); ++worker_id) {
    auto observations = std::move(follower_observations_[worker_id]);
    follower_observations_[worker_id].clear();
    if (!observations.empty() && worker_id < workers_.size()) {
      td::actor::send_closure(workers_[worker_id], &NativeLoadWorker::observe_canonical_transfers,
                              std::move(observations));
    }
  }
  bool repeat_immediately_for_measure_end = false;
  if (!workers_complete_) {
    auto issue_end_system = start_system_at_ + options_.ramp_seconds +
                            options_.warmup_seconds + options_.duration_seconds;
    bool may_capture_measure_end = started_ &&
                                   follower_poll_started_system_at_ >= issue_end_system;
    for (auto& worker : workers_) {
      td::actor::send_closure(worker, &NativeLoadWorker::canonical_checkpoint,
                              may_capture_measure_end);
    }
    if (started_ && td::Clocks::system() >= issue_end_system && !may_capture_measure_end) {
      // This poll overlapped the measurement boundary.  Its older MC anchor
      // cannot define the end snapshot, so immediately obtain a post-boundary
      // anchor instead of waiting another configured interval.
      repeat_immediately_for_measure_end = true;
    }
  }
  follower_query_active_ = false;
  next_follower_poll_at_ = td::Time::now() +
                           (repeat_immediately_for_measure_end ? 0.0
                                                               : options_.canonical_poll_seconds);
  if (!started_ && follower_start_catchup_requested_) {
    follower_start_catchup_requested_ = false;
    follower_start_catchup_complete_ = true;
    follower_ready_ = true;
    // observe_canonical_transfers messages above precede begin() for every worker
    // because they are sent by this actor in FIFO order.
    maybe_begin();
    return;
  }
  if (workers_complete_) {
    if (final_follower_poll_) {
      finalize_workers_after_follower();
    } else {
      final_follower_poll_ = true;
      request_follower_poll(false);
    }
  }
}

void NativeLoadCoordinator::follower_error(td::Status error) {
  ++follower_stats_.errors;
  follower_poll_delta_ = {};
  follower_poll_measure_second_counts_.clear();
  follower_poll_block_second_counts_.clear();
  for (auto& observations : follower_observations_) {
    observations.clear();
  }
  follower_query_active_ = false;
  if (!started_ && !follower_ready_) {
    worker_failed(0, error.to_string());
    return;
  }
  LOG(ERROR) << "canonical block follower: " << error;
  next_follower_poll_at_ = td::Time::now() + options_.canonical_poll_seconds;
  if (workers_complete_) {
    if (final_follower_poll_) {
      finalize_workers_after_follower();
    } else {
      final_follower_poll_ = true;
      request_follower_poll(false);
    }
  }
}

void NativeLoadCoordinator::finalize_workers_after_follower() {
  if (finalizing_workers_) {
    return;
  }
  finalizing_workers_ = true;
  for (auto& worker : workers_) {
    // Any observations from the just-completed poll were enqueued immediately
    // before this message by the same coordinator actor.
    td::actor::send_closure(worker, &NativeLoadWorker::finalize_after_canonical_poll);
  }
}

void NativeLoadCoordinator::finish_process() {
  report(true);
  auto total = aggregate();
  if (options_.canonical_block_follower &&
      (follower_stats_.errors || total.canonical_hash_conflicts ||
       total.duplicate_nonce_conflicts || total.external_nonce_conflicts)) {
    process_exit_code.store(3);
    LOG(ERROR) << "native load generator canonical result is invalid: follower_errors="
               << follower_stats_.errors << " hash_conflicts=" << total.canonical_hash_conflicts
               << " duplicate_nonce_conflicts=" << total.duplicate_nonce_conflicts
               << " external_nonce_conflicts=" << total.external_nonce_conflicts;
  } else if (total.drain_timed_out) {
    process_exit_code.store(2);
    LOG(ERROR) << "native load generator stopped with an unsettled drain timeout";
  } else {
    LOG(WARNING) << "native load generator stopped cleanly";
  }
  follower_client_.reset();
  workers_.clear();
  td::actor::SchedulerContext::get().stop();
  stop();
}

void NativeLoadWorker::start_up() {
  auto status = initialize();
  if (status.is_error()) {
    fail(status.move_as_error());
    return;
  }
  if (options_.auto_nonce) {
    // With the block follower enabled the coordinator supplies the exact
    // masterchain block whose ShardHashes established the follower baseline.
    // Starting an independent getMasterchainInfo scan here could observe an
    // older account state and permanently skip the intervening shard blocks.
    if (!options_.canonical_block_follower) {
      start_scan(ScanKind::startup);
    }
  } else {
    notify_ready();
  }
}

void NativeLoadWorker::discover_startup_nonces(ton::BlockIdExt ref_mc) {
  if (failed_ || finished_ || !options_.auto_nonce || !options_.canonical_block_follower) {
    return;
  }
  if (scan_kind_ != ScanKind::none || started_ || !ref_mc.is_valid_full()) {
    fail(td::Status::Error("invalid fixed-anchor startup nonce discovery state"));
    return;
  }
  scan_kind_ = ScanKind::startup;
  scan_ref_mc_ = ref_mc;
  scan_failures_ = 0;
  scan_items_.clear();
  for (std::size_t i = 0; i < wallets_.size(); ++i) {
    scan_items_.push_back(ScanItem{i, 0});
  }
  pump_scan();
}

td::Status NativeLoadWorker::initialize() {
  clients_.reserve(options_.connections);
  td::uint32 per_client = options_.max_inflight / options_.connections;
  td::uint32 per_client_extra = options_.max_inflight % options_.connections;
  // Start near a configurable bandwidth-delay product.  A one-second default
  // matches the observed physical-validator admission RTT and avoids making a
  // max-TPS run spend minutes in additive growth below its offered target.
  double guessed_window = options_.target_tps > 0.0
                              ? std::max(16.0, options_.target_tps *
                                                  options_.adaptive_initial_rtt_seconds /
                                                  options_.connections)
                              : 256.0;
  for (td::uint32 i = 0; i < options_.connections; ++i) {
    ClientSlot slot;
    slot.actor = liteclient::ExtClient::create(servers_, nullptr);
    slot.hard_limit = per_client + (i < per_client_extra ? 1u : 0u);
    slot.cwnd = options_.adaptive_inflight ? std::min<double>(slot.hard_limit, guessed_window)
                                           : static_cast<double>(slot.hard_limit);
    stats_.initial_congestion_window += slot.cwnd;
    clients_.push_back(std::move(slot));
  }
  signers_.reserve(options_.signers);
  for (td::uint32 i = 0; i < options_.signers; ++i) {
    signers_.push_back(td::actor::create_actor<Signer>(PSTRING() << "native-load-signer-" << worker_id_));
  }

  wallets_.reserve(options_.sources);
  for (td::uint32 i = 0; i < options_.sources; ++i) {
    auto source_index = options_.source_offset + i;
    auto prefix = options_.wallet_dir + "/source-" + std::to_string(source_index);
    auto destination = options_.wallet_dir + "/dest-" + std::to_string(source_index) + ".pub";
    TRY_RESULT(private_data, td::read_file(prefix + ".pk"));
    if (private_data.size() != td::Ed25519::PrivateKey::LENGTH) {
      return td::Status::Error(PSLICE() << prefix << ".pk must contain exactly 32 raw private-key bytes");
    }
    td::Ed25519::PrivateKey private_key{td::SecureString(private_data.as_slice())};
    TRY_RESULT(public_key, private_key.get_public_key());
    auto public_bytes = public_key.as_octet_string();
    ton::StdSmcAddress source;
    source.as_slice().copy_from(public_bytes);
    TRY_RESULT(stored_source, read_address(prefix + ".pub"));
    if (source != stored_source) {
      return td::Status::Error(PSLICE() << prefix << ".pk and .pub do not match");
    }
    TRY_RESULT(prepared, private_key.prepare());
    TRY_RESULT(destination_address, read_address(destination));
    Wallet wallet;
    wallet.source = source;
    wallet.destination = destination_address;
    wallet.private_key = std::move(prepared);
    wallet.next_nonce = options_.start_nonce;
    wallet.anchored_nonce = options_.start_nonce;
    wallet.run_start_nonce = options_.start_nonce;
    wallet.sampled = i < sample_sources_;
    wallets_.push_back(std::move(wallet));
  }
  std::vector<ton::StdSmcAddress> sources;
  sources.reserve(wallets_.size());
  for (const auto& wallet : wallets_) {
    sources.push_back(wallet.source);
  }
  td::actor::send_closure(coordinator_, &NativeLoadCoordinator::register_sources, worker_id_,
                          std::move(sources));
  for (std::size_t i = 0; i < wallets_.size(); ++i) {
    enqueue_available_wallet(i);
  }
  return td::Status::OK();
}

void NativeLoadWorker::notify_ready() {
  td::actor::send_closure(coordinator_, &NativeLoadCoordinator::worker_ready, worker_id_);
}

void NativeLoadWorker::fail(td::Status error) {
  if (failed_) {
    return;
  }
  failed_ = true;
  td::actor::send_closure(coordinator_, &NativeLoadCoordinator::worker_failed, worker_id_, error.to_string());
  stop();
}

void NativeLoadWorker::alarm() {
  if (failed_ || finished_) {
    return;
  }
  if (stop_requested) {
    request_graceful_stop();
  }
  if (started_) {
    auto now = td::Time::now();
    update_phase(now);
    auto issue_end = start_at_ + options_.ramp_seconds + options_.warmup_seconds + options_.duration_seconds;
    if (!sending_done_ && options_.auto_nonce && !options_.canonical_block_follower &&
        scan_kind_ == ScanKind::none && now >= next_sample_at_ &&
        now + options_.finality_poll_seconds < issue_end) {
      request_scan(ScanKind::sample);
      next_sample_at_ = now + options_.finality_poll_seconds;
    }
    pump();
    maybe_finish();
    if (now >= next_publish_at_) {
      publish_stats();
      next_publish_at_ = now + options_.report_interval;
    }
  }
  if (!failed_ && !finished_) {
    alarm_timestamp() = td::Timestamp::in(0.01);
  }
}

void NativeLoadWorker::update_phase(double now) {
  if (!steady_started_ && now >= start_at_ + options_.ramp_seconds + options_.warmup_seconds) {
    steady_started_ = true;
    stats_.steady_started = true;
    for (auto& wallet : wallets_) {
      wallet.steady_start_nonce = wallet.next_nonce;
      wallet.steady_end_nonce = wallet.next_nonce;
    }
  }
  auto issue_end = start_at_ + options_.ramp_seconds + options_.warmup_seconds + options_.duration_seconds;
  if (!sending_done_ && now >= issue_end) {
    begin_drain(now);
  }
}

void NativeLoadWorker::update_tokens(double now) {
  if (options_.target_tps <= 0.0 || now <= last_token_at_) {
    last_token_at_ = now;
    return;
  }
  auto rate_at = [&](double at) {
    auto elapsed = std::max(0.0, at - start_at_);
    if (options_.ramp_seconds && elapsed < options_.ramp_seconds) {
      return options_.target_tps * elapsed / options_.ramp_seconds;
    }
    return options_.target_tps;
  };
  auto old_rate = rate_at(last_token_at_);
  auto new_rate = rate_at(now);
  pacing_tokens_ += (old_rate + new_rate) * 0.5 * (now - last_token_at_);
  auto burst_cap = std::max(1.0, new_rate * 0.10);
  pacing_tokens_ = std::min(pacing_tokens_, burst_cap);
  last_token_at_ = now;
}

bool NativeLoadWorker::is_measure_phase(double now) const {
  auto begin = start_at_ + options_.ramp_seconds + options_.warmup_seconds;
  auto end = begin + options_.duration_seconds;
  return now >= begin && now < end;
}

bool NativeLoadWorker::can_issue(double now) const {
  if (!started_ || sending_done_ || now < start_at_) {
    return false;
  }
  if (options_.max_canonical_backlog &&
      (options_.auto_nonce || options_.canonical_block_follower) &&
      canonical_backlog_ >= options_.max_canonical_backlog) {
    return false;
  }
  return options_.target_tps <= 0.0 || pacing_tokens_ >= 1.0;
}

bool NativeLoadWorker::source_backlog_full(const Wallet& wallet) const {
  if (wallet.next_nonce < wallet.anchored_nonce) {
    return false;
  }
  auto limit = options_.max_source_canonical_backlog
                   ? std::min<td::uint64>(options_.max_source_canonical_backlog,
                                          max_native_nonce_diff)
                   : max_native_nonce_diff;
  return wallet.next_nonce - wallet.anchored_nonce >= limit;
}

bool NativeLoadWorker::task_is_active(const std::shared_ptr<TransferTask>& task) const {
  if (!task || task->wallet_idx >= wallets_.size()) {
    return false;
  }
  const auto& tasks = wallets_[task->wallet_idx].tasks;
  auto it = tasks.find(task->transfer.nonce);
  return it != tasks.end() && it->second == task;
}

void NativeLoadWorker::update_backpressure_state(double now) {
  bool paused = !sending_done_ && options_.max_canonical_backlog &&
                (options_.auto_nonce || options_.canonical_block_follower) &&
                canonical_backlog_ >= options_.max_canonical_backlog;
  if (paused == canonical_backpressure_paused_) {
    return;
  }
  if (paused) {
    canonical_backpressure_started_at_ = now;
    ++stats_.canonical_backpressure_events;
  } else {
    canonical_backpressure_accumulated_ += std::max(0.0, now - canonical_backpressure_started_at_);
  }
  canonical_backpressure_paused_ = paused;
}

td::optional<std::size_t> NativeLoadWorker::find_available_wallet() {
  while (!available_wallets_.empty()) {
    auto entry = available_wallets_.front();
    available_wallets_.pop_front();
    auto& wallet = wallets_[entry.wallet_idx];
    if (!wallet.available_queued || wallet.available_generation != entry.generation) {
      continue;
    }
    wallet.available_queued = false;
    if (!wallet.disabled && !source_backlog_full(wallet)) {
      return entry.wallet_idx;
    }
    if (source_backlog_full(wallet)) {
      ++stats_.source_backpressure_stalls;
    }
  }
  return {};
}

void NativeLoadWorker::enqueue_available_wallet(std::size_t wallet_idx) {
  auto& wallet = wallets_[wallet_idx];
  if (wallet.disabled || wallet.available_queued || source_backlog_full(wallet)) {
    return;
  }
  wallet.available_queued = true;
  ++wallet.available_generation;
  available_wallets_.push_back(AvailableWallet{wallet_idx, wallet.available_generation});
}

void NativeLoadWorker::invalidate_available_wallet(std::size_t wallet_idx) {
  auto& wallet = wallets_[wallet_idx];
  wallet.available_queued = false;
  ++wallet.available_generation;
}

void NativeLoadWorker::pump() {
  if (!started_ || failed_ || finished_) {
    return;
  }
  auto now = td::Time::now();
  // Callback-driven pumping can cross a phase boundary between 10 ms alarms.
  // Establish the immutable measured nonce range before issuing at that time,
  // and stop issuing immediately when the measurement window closes.
  update_phase(now);
  update_tokens(now);
  update_backpressure_state(now);
  while (!retry_tasks_.empty() && retry_tasks_.begin()->first <= now) {
    auto task = retry_tasks_.begin()->second;
    retry_tasks_.erase(retry_tasks_.begin());
    if (task->state != TaskState::retry_wait || !task_is_active(task)) {
      continue;
    }
    if (task->boc.empty()) {
      sign_task(std::move(task), false);
    } else if (task->transfer.valid_until <= static_cast<ton::UnixTime>(td::Clocks::system() + 1)) {
      sign_task(std::move(task), true);
    } else {
      task->state = TaskState::ready;
      ready_tasks_.push_back(std::move(task));
    }
  }
  dispatch_ready();

  while (active_tasks_ < options_.max_inflight && can_issue(now)) {
    auto wallet_idx = find_available_wallet();
    if (!wallet_idx) {
      break;
    }
    // `find_available_wallet()` and callback traffic can straddle the exact
    // end timestamp. Re-read time before consuming a nonce so no post-window
    // offer is classified from the previous loop iteration's timestamp.
    now = td::Time::now();
    update_phase(now);
    update_tokens(now);
    if (!can_issue(now)) {
      enqueue_available_wallet(wallet_idx.value());
      break;
    }
    bool measured = is_measure_phase(now);
    auto nonce = wallets_[wallet_idx.value()].next_nonce++;
    ++canonical_backlog_;
    create_transfer(wallet_idx.value(), nonce, measured, false);
    enqueue_available_wallet(wallet_idx.value());
    if (options_.target_tps > 0.0) {
      pacing_tokens_ -= 1.0;
    }
    now = td::Time::now();
    update_phase(now);
    update_tokens(now);
  }
  update_backpressure_state(td::Time::now());
  dispatch_ready();
}

void NativeLoadWorker::create_transfer(std::size_t wallet_idx, td::uint64 nonce, bool measured, bool repair) {
  auto& wallet = wallets_[wallet_idx];
  CHECK(!wallet.disabled && !wallet.tasks.count(nonce));
  auto task = std::make_shared<TransferTask>();
  task->wallet_idx = wallet_idx;
  task->transfer.src = wallet.source;
  task->transfer.dst = wallet.destination;
  task->transfer.amount = options_.amount;
  task->transfer.fee = options_.fee;
  task->transfer.nonce = nonce;
  auto expires = td::Clocks::system() + options_.valid_for_seconds;
  if (expires >= std::numeric_limits<ton::UnixTime>::max()) {
    fail(td::Status::Error("valid_until overflows uint32"));
    return;
  }
  task->transfer.valid_until = static_cast<ton::UnixTime>(expires);
  task->first_issued_at = td::Time::now();
  task->measured = measured;
  task->repair = repair;
  wallet.tasks.emplace(nonce, task);
  ++active_tasks_;
  if (repair) {
    wallet.last_repair_nonce = nonce;
    wallet.last_repair_at = task->first_issued_at;
    ++stats_.repair_offered;
  } else {
    ++stats_.offered;
    if (measured) {
      // Record cohort membership when the nonce is issued. Canonical proofs
      // can arrive during the measurement window; waiting until begin_drain()
      // to close this range would erase an exact hash before it could ever be
      // counted as a measured transfer.
      CHECK(nonce != std::numeric_limits<td::uint64>::max());
      wallet.steady_end_nonce = std::max(wallet.steady_end_nonce, nonce + 1);
      CHECK(steady_started_ &&
            nonce_in_half_open_cohort(nonce, wallet.steady_start_nonce,
                                      wallet.steady_end_nonce));
      ++stats_.steady_offered;
      if (wallet.sampled) {
        wallet.samples.push_back(AnchorSample{nonce, task->first_issued_at});
      }
    }
  }
  sign_task(std::move(task), false);
}

void NativeLoadWorker::sign_task(std::shared_ptr<TransferTask> task, bool resign) {
  task->state = TaskState::signing;
  task->sign_started_at = td::Time::now();
  task->boc = {};
  task->transfer.signature.clear();
  if (resign) {
    auto expires = td::Clocks::system() + options_.valid_for_seconds;
    if (expires >= std::numeric_limits<ton::UnixTime>::max()) {
      fail(td::Status::Error("valid_until overflows uint32"));
      return;
    }
    task->transfer.valid_until = static_cast<ton::UnixTime>(expires);
    task->attempts = 0;
    task->retry_exhaustion_counted = false;
    task->resigned_after_expiry = true;
    ++stats_.resigned;
  }
  auto promise = td::PromiseCreator::lambda(
      [self = actor_id(this), task](td::Result<SignedTransfer> message) mutable {
        td::actor::send_closure(self, &NativeLoadWorker::on_signed, std::move(task), std::move(message));
      });
  auto& signer = signers_[signer_cursor_++ % signers_.size()];
  td::actor::send_closure(signer, &Signer::sign, task->transfer,
                          options_.chain_domain,
                          wallets_[task->wallet_idx].private_key, std::move(promise));
  ++signing_;
}

void NativeLoadWorker::on_signed(std::shared_ptr<TransferTask> task, td::Result<SignedTransfer> message) {
  CHECK(signing_ > 0);
  --signing_;
  ++stats_.sign_operations;
  stats_.signing_latency.observe_seconds(td::Time::now() - task->sign_started_at);
  if (finished_) {
    return;
  }
  if (!task_is_active(task)) {
    if (options_.submit_batch_size == 1) {
      pump();
    } else {
      alarm_timestamp().relax(td::Timestamp::in(0.001));
    }
    return;
  }
  if (message.is_error()) {
    ++stats_.sign_errors;
    LOG(ERROR) << "worker " << worker_id_ << " signing failed for nonce " << task->transfer.nonce << ": "
               << message.error();
    schedule_retry(std::move(task), std::max(0.001, options_.retry_backoff_ms / 1000.0));
  } else {
    auto signed_transfer = message.move_as_ok();
    task->boc = std::move(signed_transfer.boc);
    auto& hashes = wallets_[task->wallet_idx].expected_hashes[task->transfer.nonce];
    if (std::find(hashes.begin(), hashes.end(), signed_transfer.external_hash) == hashes.end()) {
      hashes.push_back(signed_transfer.external_hash);
    }
    task->state = TaskState::ready;
    ready_tasks_.push_back(std::move(task));
  }
  // Let signer completions coalesce until the next worker tick when batching
  // is enabled. Dispatching on every completion degenerates nominal 64-message
  // batches into one-message batch queries.
  if (options_.submit_batch_size == 1) {
    pump();
  } else {
    alarm_timestamp().relax(td::Timestamp::in(0.001));
  }
  maybe_finish();
}

td::optional<std::size_t> NativeLoadWorker::select_client() const {
  td::optional<std::size_t> selected;
  double best_load = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < clients_.size(); ++i) {
    auto capacity = options_.adaptive_inflight
                        ? std::max<td::uint32>(1, static_cast<td::uint32>(std::floor(clients_[i].cwnd)))
                        : clients_[i].hard_limit;
    capacity = std::min(capacity, clients_[i].hard_limit);
    if (clients_[i].inflight >= capacity) {
      continue;
    }
    auto load = static_cast<double>(clients_[i].inflight) / capacity;
    if (!selected || load < best_load) {
      selected = i;
      best_load = load;
    }
  }
  return selected;
}

td::uint32 NativeLoadWorker::client_available_capacity(std::size_t client_idx) const {
  auto capacity = options_.adaptive_inflight
                      ? std::max<td::uint32>(1, static_cast<td::uint32>(std::floor(clients_[client_idx].cwnd)))
                      : clients_[client_idx].hard_limit;
  capacity = std::min(capacity, clients_[client_idx].hard_limit);
  return clients_[client_idx].inflight < capacity ? capacity - clients_[client_idx].inflight : 0;
}

bool NativeLoadWorker::ready_for_first_submission(
    const std::shared_ptr<TransferTask>& task) const {
  if (task->ever_submitted) {
    return true;
  }
  const auto& tasks = wallets_[task->wallet_idx].tasks;
  for (auto it = tasks.begin(); it != tasks.end() && it->first < task->transfer.nonce; ++it) {
    if (!it->second->ever_submitted) {
      return false;
    }
  }
  return true;
}

std::shared_ptr<NativeLoadWorker::TransferTask>
NativeLoadWorker::take_dispatchable_ready_task() {
  auto candidates = ready_tasks_.size();
  while (candidates-- && !ready_tasks_.empty()) {
    auto task = ready_tasks_.front();
    ready_tasks_.pop_front();
    if (task->state != TaskState::ready || !task_is_active(task)) {
      continue;
    }
    if (ready_for_first_submission(task)) {
      return task;
    }
    ready_tasks_.push_back(std::move(task));
  }
  return {};
}

void NativeLoadWorker::dispatch_ready() {
  while (!ready_tasks_.empty() && inflight_ < options_.max_inflight) {
    auto client_idx = select_client();
    if (!client_idx) {
      break;
    }
    if (options_.submit_batch_size == 1) {
      auto task = take_dispatchable_ready_task();
      if (!task) {
        break;
      }
      send_task(std::move(task), client_idx.value());
      continue;
    }
    auto capacity = std::min<td::uint64>(client_available_capacity(client_idx.value()),
                                         options_.max_inflight - inflight_);
    auto batch_limit = std::min<td::uint64>(options_.submit_batch_size, capacity);
    std::vector<std::shared_ptr<TransferTask>> tasks;
    tasks.reserve(batch_limit);
    while (tasks.size() < batch_limit) {
      auto task = take_dispatchable_ready_task();
      if (!task) {
        break;
      }
      tasks.push_back(std::move(task));
    }
    if (!tasks.empty()) {
      send_batch(std::move(tasks), client_idx.value());
    } else {
      break;
    }
  }
}

void NativeLoadWorker::send_task(std::shared_ptr<TransferTask> task, std::size_t client_idx) {
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_sendMessage>(task->boc.clone()), true);
  auto promise = td::PromiseCreator::lambda(
      [self = actor_id(this), task, client_idx](td::Result<td::BufferSlice> result) mutable {
        td::actor::send_closure(self, &NativeLoadWorker::on_result, std::move(task), client_idx,
                                std::move(result));
      });
  task->state = TaskState::inflight;
  task->last_sent_at = td::Time::now();
  if (!task->ever_submitted) {
    task->ever_submitted = true;
    if (task->repair) {
      ++stats_.repair_submitted;
    } else {
      ++stats_.submitted;
      if (task->measured) {
        ++stats_.steady_submitted;
      }
    }
  }
  ++task->attempts;
  ++stats_.wire_attempts;
  ++stats_.wire_queries;
  ++inflight_;
  ++clients_[client_idx].inflight;
  td::actor::send_closure(clients_[client_idx].actor, &liteclient::ExtClient::send_query, "native-load",
                          envelope_query(std::move(query)), td::Timestamp::in(options_.query_timeout),
                          std::move(promise));
}

void NativeLoadWorker::send_batch(std::vector<std::shared_ptr<TransferTask>> tasks,
                                  std::size_t client_idx) {
  CHECK(!tasks.empty() && tasks.size() <= options_.submit_batch_size);
  std::vector<td::BufferSlice> bodies;
  bodies.reserve(tasks.size());
  auto sent_at = td::Time::now();
  for (auto& task : tasks) {
    bodies.push_back(task->boc.clone());
    task->state = TaskState::inflight;
    task->last_sent_at = sent_at;
    if (!task->ever_submitted) {
      task->ever_submitted = true;
      if (task->repair) {
        ++stats_.repair_submitted;
      } else {
        ++stats_.submitted;
        if (task->measured) {
          ++stats_.steady_submitted;
        }
      }
    }
    ++task->attempts;
  }
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_sendMessageBatch>(std::move(bodies)), true);
  auto promise = td::PromiseCreator::lambda(
      [self = actor_id(this), tasks, client_idx](td::Result<td::BufferSlice> result) mutable {
        td::actor::send_closure(self, &NativeLoadWorker::on_batch_result, std::move(tasks), client_idx,
                                std::move(result));
      });
  stats_.wire_attempts += tasks.size();
  ++stats_.wire_queries;
  ++stats_.wire_batches;
  stats_.wire_batch_messages += tasks.size();
  stats_.max_wire_batch_size = std::max<td::uint64>(stats_.max_wire_batch_size, tasks.size());
  inflight_ += tasks.size();
  clients_[client_idx].inflight += tasks.size();
  td::actor::send_closure(clients_[client_idx].actor, &liteclient::ExtClient::send_query,
                          "native-load-batch", envelope_query(std::move(query)),
                          td::Timestamp::in(options_.query_timeout), std::move(promise));
}

void NativeLoadWorker::on_result(std::shared_ptr<TransferTask> task, std::size_t client_idx,
                                 td::Result<td::BufferSlice> result) {
  CHECK(inflight_ > 0 && client_idx < clients_.size() && clients_[client_idx].inflight > 0);
  --inflight_;
  --clients_[client_idx].inflight;
  stats_.request_latency.observe_seconds(td::Time::now() - task->last_sent_at);
  if (finished_) {
    return;
  }
  if (!task_is_active(task)) {
    pump();
    maybe_finish();
    return;
  }
  bool transport_error = result.is_error();
  auto data = unwrap_lite_result(std::move(result));
  if (data.is_error()) {
    auto error = data.move_as_error();
    handle_task_error(std::move(task), client_idx, std::move(error),
                      transport_error ? ErrorOrigin::transport : ErrorOrigin::server);
  } else {
    auto parsed = ton::fetch_tl_object<ton::lite_api::liteServer_sendMsgStatus>(data.move_as_ok(), true);
    if (parsed.is_error()) {
      ++stats_.parse_errors;
      handle_task_error(std::move(task), client_idx, parsed.move_as_error(), ErrorOrigin::parse);
    } else if (parsed.move_as_ok()->status_ == 1) {
      if (options_.adaptive_inflight) {
        auto& client = clients_[client_idx];
        client.cwnd = std::min<double>(client.hard_limit, client.cwnd + 1.0 / std::max(1.0, client.cwnd));
      }
      accept_task(std::move(task), TaskResolution::admitted);
    } else {
      ++stats_.rejected_other;
      reject_task(std::move(task), "sendMessage returned a non-success status");
    }
  }
  pump();
  maybe_finish();
}

void NativeLoadWorker::on_batch_result(std::vector<std::shared_ptr<TransferTask>> tasks,
                                       std::size_t client_idx, td::Result<td::BufferSlice> result) {
  CHECK(!tasks.empty() && client_idx < clients_.size() && inflight_ >= tasks.size() &&
        clients_[client_idx].inflight >= tasks.size());
  inflight_ -= tasks.size();
  clients_[client_idx].inflight -= tasks.size();
  auto now = td::Time::now();
  for (const auto& task : tasks) {
    stats_.request_latency.observe_seconds(now - task->last_sent_at);
  }
  if (finished_) {
    return;
  }
  bool transport_error = result.is_error();
  auto data = unwrap_lite_result(std::move(result));
  if (data.is_error()) {
    auto error = data.move_as_error();
    auto code = error.code();
    auto message = error.message().str();
    for (auto& task : tasks) {
      if (task_is_active(task)) {
        handle_task_error(std::move(task), client_idx, td::Status::Error(code, message),
                          transport_error ? ErrorOrigin::transport : ErrorOrigin::server);
      }
    }
    pump();
    maybe_finish();
    return;
  }
  auto parsed = ton::fetch_tl_object<ton::lite_api::liteServer_sendMsgStatusBatch>(data.move_as_ok(), true);
  if (parsed.is_error()) {
    auto error = parsed.move_as_error();
    auto code = error.code();
    auto message = error.message().str();
    stats_.parse_errors += tasks.size();
    for (auto& task : tasks) {
      if (task_is_active(task)) {
        handle_task_error(std::move(task), client_idx, td::Status::Error(code, message), ErrorOrigin::parse);
      }
    }
    pump();
    maybe_finish();
    return;
  }
  auto statuses = parsed.move_as_ok();
  if (statuses->results_.size() != tasks.size()) {
    stats_.parse_errors += tasks.size();
    for (auto& task : tasks) {
      if (task_is_active(task)) {
        handle_task_error(std::move(task), client_idx,
                          td::Status::Error("sendMessageBatch result count mismatch"), ErrorOrigin::parse);
      }
    }
    pump();
    maybe_finish();
    return;
  }
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    auto task = std::move(tasks[i]);
    if (!task_is_active(task)) {
      continue;
    }
    auto& status = statuses->results_[i];
    if (status && status->status_ == 1) {
      if (options_.adaptive_inflight) {
        auto& client = clients_[client_idx];
        client.cwnd = std::min<double>(client.hard_limit,
                                       client.cwnd + 1.0 / std::max(1.0, client.cwnd));
      }
      accept_task(std::move(task), TaskResolution::admitted);
    } else if (status && status->status_ == 0) {
      handle_task_error(std::move(task), client_idx,
                        td::Status::Error(status->code_, status->message_), ErrorOrigin::server);
    } else {
      ++stats_.rejected_other;
      reject_task(std::move(task), "sendMessageBatch returned an invalid per-message status");
    }
  }
  pump();
  maybe_finish();
}

void NativeLoadWorker::handle_task_error(std::shared_ptr<TransferTask> task, std::size_t client_idx,
                                         td::Status error, ErrorOrigin origin) {
  auto text = error.to_string();
  auto lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  auto contains = [&](td::Slice needle) { return lower.find(needle.str()) != std::string::npos; };
  bool timeout = error.code() == ton::ErrorCode::timeout || contains("timeout");
  bool full = contains("mempool is full");
  bool rate_limit = contains("per-address") || contains("too many external") || contains("rate limit");
  bool duplicate = contains("duplicate native nonce") || contains("already exists");
  bool too_old = contains("too old native nonce");
  bool expired = contains("valid_until") || contains("expired");
  bool too_new = contains("too new native nonce");
  bool not_ready = error.code() == ton::ErrorCode::notready || contains("not ready") ||
                   contains("still in flight");
  // ErrorCode::notready may explain that the canonical account state has not
  // caught up with a finalized balance.  It is transient state lag, not an
  // insufficient-balance verdict, even though the diagnostic contains the
  // word "balance".
  bool balance = !not_ready && (contains("insufficient") || contains("balance"));
  bool invalid = !not_ready && (contains("signature") || contains("wrong source") ||
                                contains("must be balance-only") || contains("overflow"));
  if (timeout) {
    ++stats_.timeouts;
  } else if (origin == ErrorOrigin::server) {
    ++stats_.server_errors;
  } else if (origin == ErrorOrigin::transport) {
    ++stats_.transport_errors;
  }
  if (options_.adaptive_inflight && (timeout || full || rate_limit || not_ready)) {
    auto now = td::Time::now();
    auto& client = clients_[client_idx];
    // A burst of failures from one old window is one congestion event, not
    // hundreds of independent reasons to repeatedly halve the same window.
    if (now - client.last_decrease_at >= 0.1) {
      client.cwnd = std::max(1.0, client.cwnd * 0.5);
      client.last_decrease_at = now;
    }
  }
  if (duplicate) {
    if (task->resigned_after_expiry) {
      // The old admitted variant can remain reserved until the node observes
      // its expiry and removes it. This duplicate is an expected handover
      // race between two hashes signed by this run; retain both expected
      // hashes and retry the replacement without claiming admission.
      schedule_retry(std::move(task), std::max(0.25, options_.repair_cooldown_seconds));
      return;
    }
    ++stats_.duplicate_nonce_conflicts;
    ++stats_.canonical_hash_conflicts;
    ++stats_.rejected;
    ++stats_.rejected_nonce;
    disable_wallet_for_conflict(task->wallet_idx,
                                "a different or unresolved message already owns this native nonce");
    return;
  }
  if (too_old) {
    ++stats_.canonical_inferred_too_old;
    if (!options_.canonical_block_follower) {
      ++stats_.canonical_hash_conflicts;
      ++stats_.rejected;
      ++stats_.rejected_nonce;
      disable_wallet_for_conflict(task->wallet_idx,
                                  "too-old nonce cannot be attributed without the canonical block follower");
      return;
    }
    // A too-old response is only a hint that some transfer advanced the
    // account. Keep this exact task unresolved until the proof follower sees
    // the canonical external hash; never turn the hint into canonical TPS.
    schedule_retry(std::move(task), std::max(0.25, options_.repair_cooldown_seconds));
    return;
  }
  if (expired) {
    ++stats_.rejected_expired;
    sign_task(std::move(task), true);
    return;
  }
  if (balance || invalid) {
    if (balance) {
      ++stats_.rejected_balance;
    } else {
      ++stats_.rejected_invalid;
    }
    reject_task(std::move(task), text);
    return;
  }
  if (full) {
    ++stats_.rejected_full;
  }
  if (rate_limit) {
    ++stats_.rejected_rate_limit;
  }
  if (too_new) {
    ++stats_.rejected_nonce;
  }
  auto exponent = std::min<td::uint32>(task->attempts ? task->attempts - 1 : 0, 10);
  auto delay = options_.retry_backoff_ms / 1000.0 * static_cast<double>(1u << exponent);
  if (task->attempts > options_.max_retries) {
    if (!task->retry_exhaustion_counted) {
      task->retry_exhaustion_counted = true;
      ++stats_.retry_exhausted;
    }
    task->attempts = 0;
    delay = std::max(delay, options_.finality_poll_seconds);
  }
  schedule_retry(std::move(task), std::max(0.001, delay));
}

void NativeLoadWorker::schedule_retry(std::shared_ptr<TransferTask> task, double delay_seconds) {
  task->state = TaskState::retry_wait;
  task->retry_at = td::Time::now() + delay_seconds;
  retry_tasks_.emplace(task->retry_at, std::move(task));
  ++stats_.retries;
}

void NativeLoadWorker::accept_task(std::shared_ptr<TransferTask> task, TaskResolution resolution) {
  auto& wallet = wallets_[task->wallet_idx];
  auto it = wallet.tasks.find(task->transfer.nonce);
  if (it == wallet.tasks.end() || it->second != task) {
    return;
  }
  if (resolution != TaskResolution::admitted) {
    ++stats_.accepted_inferred;
    ++stats_.proof_observed_resolved;
    wallet.admitted_tasks.erase(task->transfer.nonce);
  } else {
    if (!task->admission_counted) {
      task->admission_counted = true;
      ++stats_.mempool_accepted;
      if (task->repair) {
        ++stats_.repair_accepted;
      } else if (task->measured) {
        ++stats_.steady_mempool_accepted;
      }
    } else {
      ++stats_.repeat_admission_successes;
    }
    // A successful response for a replacement means the new exact bytes now
    // own the admission. Future duplicate-nonce responses are conflicts again,
    // not part of the bounded old-hash expiry handover.
    task->resigned_after_expiry = false;
    wallet.admitted_tasks[task->transfer.nonce] = task;
  }
  task->state = TaskState::resolved;
  CHECK(active_tasks_ > 0);
  --active_tasks_;
  wallet.tasks.erase(it);
  enqueue_available_wallet(task->wallet_idx);
}

void NativeLoadWorker::disable_wallet_for_conflict(std::size_t wallet_idx, td::Slice reason) {
  if (wallet_idx >= wallets_.size()) {
    return;
  }
  auto& wallet = wallets_[wallet_idx];
  if (wallet.disabled) {
    return;
  }
  wallet.disabled = true;
  invalidate_available_wallet(wallet_idx);
  CHECK(active_tasks_ >= wallet.tasks.size());
  active_tasks_ -= wallet.tasks.size();
  for (auto& [nonce, pending] : wallet.tasks) {
    static_cast<void>(nonce);
    pending->state = TaskState::resolved;
  }
  wallet.tasks.clear();
  LOG(ERROR) << "worker " << worker_id_ << " disabled native source "
             << wallet.source.to_hex() << " after canonical attribution conflict: " << reason;
  update_backpressure_state(td::Time::now());
}

void NativeLoadWorker::reject_task(std::shared_ptr<TransferTask> task, td::Slice reason) {
  auto& wallet = wallets_[task->wallet_idx];
  auto it = wallet.tasks.find(task->transfer.nonce);
  if (it == wallet.tasks.end() || it->second != task) {
    return;
  }
  ++stats_.rejected;
  wallet.disabled = true;
  invalidate_available_wallet(task->wallet_idx);
  CHECK(active_tasks_ >= wallet.tasks.size());
  active_tasks_ -= wallet.tasks.size();
  for (auto& [nonce, pending] : wallet.tasks) {
    static_cast<void>(nonce);
    pending->state = TaskState::resolved;
  }
  wallet.tasks.clear();
  LOG(ERROR) << "worker " << worker_id_ << " permanently rejected source " << wallet.source.to_hex()
             << " nonce " << task->transfer.nonce << ": " << reason;
}

void NativeLoadWorker::begin_drain(double now) {
  if (sending_done_) {
    return;
  }
  sending_done_ = true;
  stats_.sending_done = true;
  update_backpressure_state(now);
  drain_started_at_ = now;
  drain_deadline_ = now + options_.drain_timeout_seconds;
  // Always leave time for at least one repair attempt inside the configured
  // drain, even when a deliberately conservative repeat cooldown is longer
  // than the drain itself.  The cooldown still applies after that first try.
  auto first_repair_delay = std::min(options_.repair_cooldown_seconds,
                                     std::max(0.25, options_.drain_timeout_seconds * 0.25));
  next_drain_scan_at_ = now + (options_.canonical_block_follower ? first_repair_delay : 0.0);
  if (!steady_started_) {
    steady_started_ = true;
    stats_.steady_started = true;
    for (auto& wallet : wallets_) {
      wallet.steady_start_nonce = wallet.next_nonce;
    }
  }
  for (auto& wallet : wallets_) {
    wallet.steady_end_nonce = wallet.next_nonce;
  }
  if (options_.auto_nonce && !options_.canonical_block_follower) {
    request_scan(ScanKind::end_boundary);
  }
}

void NativeLoadWorker::maybe_finish() {
  if (!sending_done_ || finished_) {
    return;
  }
  auto now = td::Time::now();
  if (now >= drain_deadline_) {
    if (!options_.canonical_block_follower && options_.auto_nonce &&
        !stats_.drain_snapshot_complete && !final_scan_grace_used_) {
      final_scan_grace_used_ = true;
      if (scan_kind_ == ScanKind::none) {
        request_scan(ScanKind::drain);
      }
      // Give the final snapshot at most one query timeout of grace.  This is a
      // one-shot extension; an unfinalized run cannot extend itself forever.
      drain_deadline_ = now + options_.query_timeout;
      return;
    }
    stats_.drain_timed_out = active_tasks_ != 0 || inflight_ != 0 || signing_ != 0 ||
                             ((options_.auto_nonce || options_.canonical_block_follower) &&
                              (!stats_.end_snapshot_complete || !stats_.drain_snapshot_complete ||
                               !canonical_cohorts_complete(
                                   stats_.anchored_after_drain, stats_.steady_offered,
                                   stats_.total_anchored_after_drain, stats_.offered)));
    finish();
    return;
  }
  if (options_.canonical_block_follower) {
    if (!active_tasks_ && !inflight_ && !signing_ && stats_.end_snapshot_complete &&
        stats_.drain_snapshot_complete &&
        canonical_cohorts_complete(stats_.anchored_after_drain, stats_.steady_offered,
                                   stats_.total_anchored_after_drain, stats_.offered)) {
      stats_.drain_to_anchor_seconds = std::max(0.0, now - drain_started_at_);
      finish();
    }
    return;
  }
  if (!options_.auto_nonce) {
    if (!active_tasks_ && !inflight_ && !signing_) {
      finish();
    }
    return;
  }
  if (scan_kind_ == ScanKind::none && now >= next_drain_scan_at_) {
    if (!end_snapshot_finished_) {
      request_scan(ScanKind::end_boundary);
    } else if (!stats_.drain_snapshot_complete ||
               !canonical_cohorts_complete(stats_.anchored_after_drain, stats_.steady_offered,
                                           stats_.total_anchored_after_drain, stats_.offered)) {
      request_scan(ScanKind::drain);
    }
  }
  if (active_tasks_ || inflight_ || signing_) {
    return;
  }
  if (stats_.drain_snapshot_complete &&
      canonical_cohorts_complete(stats_.anchored_after_drain, stats_.steady_offered,
                                 stats_.total_anchored_after_drain, stats_.offered)) {
    stats_.drain_to_anchor_seconds = std::max(0.0, now - drain_started_at_);
    finish();
  }
}

void NativeLoadWorker::finish() {
  if (finished_) {
    return;
  }
  finished_ = true;
  refresh_stats();
  td::actor::send_closure(coordinator_, &NativeLoadCoordinator::worker_done, worker_id_, stats_);
  if (options_.canonical_block_follower) {
    // Keep this actor and its cohort boundaries alive through the
    // coordinator's final proof poll.  The final observations can then update
    // canonical_after_drain instead of being discarded after worker_done().
    return;
  }
  clients_.clear();
  signers_.clear();
  stop();
}

void NativeLoadWorker::refresh_stats() {
  auto now = td::Time::now();
  update_backpressure_state(now);
  stats_.inflight = inflight_;
  stats_.signing = signing_;
  stats_.ready = 0;
  stats_.retry_wait = 0;
  stats_.active_tasks = active_tasks_;
  stats_.active_sources = 0;
  stats_.congestion_window = 0.0;
  for (const auto& wallet : wallets_) {
    if (wallet.tasks.empty()) {
      continue;
    }
    ++stats_.active_sources;
    for (const auto& [nonce, task] : wallet.tasks) {
      static_cast<void>(nonce);
      if (task->state == TaskState::ready) {
        ++stats_.ready;
      } else if (task->state == TaskState::retry_wait) {
        ++stats_.retry_wait;
      }
    }
  }
  for (const auto& client : clients_) {
    stats_.congestion_window += client.cwnd;
  }
  stats_.pacing_tokens = pacing_tokens_;
  stats_.canonical_backlog = canonical_backlog_;
  stats_.canonical_backpressure_paused = canonical_backpressure_paused_;
  stats_.canonical_backpressure_seconds =
      canonical_backpressure_accumulated_ +
      (canonical_backpressure_paused_ ? std::max(0.0, now - canonical_backpressure_started_at_) : 0.0);
  stats_.interrupted = interrupted_;
  if (started_) {
    auto measure_begin = start_at_ + options_.ramp_seconds + options_.warmup_seconds;
    auto configured_end = measure_begin + options_.duration_seconds;
    auto observed_end = sending_done_ ? drain_started_at_ : now;
    stats_.measure_elapsed_seconds =
        std::max(0.0, std::min(observed_end, configured_end) - measure_begin);
  }
}

void NativeLoadWorker::publish_stats() {
  refresh_stats();
  td::actor::send_closure(coordinator_, &NativeLoadCoordinator::worker_stats, worker_id_, stats_);
}

void NativeLoadWorker::request_scan(ScanKind kind) {
  if (!options_.auto_nonce || failed_ || finished_) {
    return;
  }
  if (scan_kind_ != ScanKind::none) {
    if (static_cast<int>(kind) > static_cast<int>(pending_scan_)) {
      pending_scan_ = kind;
    }
    return;
  }
  start_scan(kind);
}

void NativeLoadWorker::start_scan(ScanKind kind) {
  scan_kind_ = kind;
  scan_failures_ = 0;
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfo>(), true);
  auto promise = td::PromiseCreator::lambda(
      [self = actor_id(this), kind](td::Result<td::BufferSlice> result) mutable {
        td::actor::send_closure(self, &NativeLoadWorker::on_scan_masterchain, kind, std::move(result));
      });
  auto& client = clients_[scan_client_cursor_++ % clients_.size()].actor;
  td::actor::send_closure(client, &liteclient::ExtClient::send_query, "native-load-anchor",
                          envelope_query(std::move(query)), td::Timestamp::in(options_.query_timeout),
                          std::move(promise));
}

void NativeLoadWorker::on_scan_masterchain(ScanKind kind, td::Result<td::BufferSlice> result) {
  if (kind != scan_kind_) {
    return;
  }
  auto data = unwrap_lite_result(std::move(result));
  if (data.is_error()) {
    ++stats_.anchor_scan_errors;
    if (kind == ScanKind::startup) {
      fail(data.move_as_error_prefix("cannot obtain startup masterchain anchor: "));
      return;
    }
    scan_kind_ = ScanKind::none;
    if (kind == ScanKind::sample) {
      next_sample_at_ = td::Time::now() + options_.finality_poll_seconds;
    } else {
      if (kind == ScanKind::end_boundary) {
        end_snapshot_finished_ = true;
        next_drain_scan_at_ = td::Time::now();
      } else {
        next_drain_scan_at_ = td::Time::now() + options_.finality_poll_seconds;
      }
    }
    if (pending_scan_ != ScanKind::none) {
      auto pending = pending_scan_;
      pending_scan_ = ScanKind::none;
      start_scan(pending);
    }
    return;
  }
  auto parsed = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfo>(data.move_as_ok(), true);
  if (parsed.is_error()) {
    ++stats_.anchor_scan_errors;
    if (kind == ScanKind::startup) {
      fail(parsed.move_as_error_prefix("cannot parse startup masterchain anchor: "));
      return;
    }
    scan_kind_ = ScanKind::none;
    if (kind == ScanKind::sample) {
      next_sample_at_ = td::Time::now() + options_.finality_poll_seconds;
    } else {
      if (kind == ScanKind::end_boundary) {
        end_snapshot_finished_ = true;
        next_drain_scan_at_ = td::Time::now();
      } else {
        next_drain_scan_at_ = td::Time::now() + options_.finality_poll_seconds;
      }
    }
    if (pending_scan_ != ScanKind::none) {
      auto pending = pending_scan_;
      pending_scan_ = ScanKind::none;
      start_scan(pending);
    }
    return;
  }
  scan_ref_mc_ = ton::create_block_id(parsed.move_as_ok()->last_);
  scan_items_.clear();
  bool full_progress_scan = kind == ScanKind::sample && !options_.canonical_block_follower &&
                            (options_.max_canonical_backlog || options_.max_source_canonical_backlog);
  if (kind == ScanKind::sample && !full_progress_scan) {
    for (std::size_t i = 0; i < wallets_.size(); ++i) {
      if (wallets_[i].sampled) {
        scan_items_.push_back(ScanItem{i, 0});
      }
    }
  } else {
    for (std::size_t i = 0; i < wallets_.size(); ++i) {
      scan_items_.push_back(ScanItem{i, 0});
    }
  }
  pump_scan();
}

void NativeLoadWorker::pump_scan() {
  auto parallelism = std::max<td::uint32>(1, std::min<td::uint32>(options_.max_inflight / 4,
                                                                  options_.connections * 16));
  while (!scan_items_.empty() && scan_inflight_ < parallelism) {
    auto item = scan_items_.front();
    scan_items_.pop_front();
    auto account = ton::create_tl_object<ton::lite_api::liteServer_accountId>(
        ton::basechainId, wallets_[item.wallet_idx].source);
    auto query = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_getAccountState>(
            ton::create_tl_lite_block_id(scan_ref_mc_), std::move(account)),
        true);
    auto promise = td::PromiseCreator::lambda(
        [self = actor_id(this), kind = scan_kind_, ref_mc = scan_ref_mc_, item](
            td::Result<td::BufferSlice> result) mutable {
          td::actor::send_closure(self, &NativeLoadWorker::on_scan_account, kind, ref_mc, item,
                                  std::move(result));
        });
    auto& client = clients_[scan_client_cursor_++ % clients_.size()].actor;
    ++scan_inflight_;
    td::actor::send_closure(client, &liteclient::ExtClient::send_query, "native-load-account-anchor",
                            envelope_query(std::move(query)), td::Timestamp::in(options_.query_timeout),
                            std::move(promise));
  }
  if (scan_items_.empty() && !scan_inflight_) {
    finish_scan();
  }
}

void NativeLoadWorker::on_scan_account(ScanKind kind, ton::BlockIdExt ref_mc, ScanItem item,
                                       td::Result<td::BufferSlice> result) {
  if (scan_inflight_) {
    --scan_inflight_;
  }
  if (kind != scan_kind_ || ref_mc != scan_ref_mc_) {
    return;
  }
  auto data = unwrap_lite_result(std::move(result));
  if (data.is_ok()) {
    auto nonce = parse_native_account_nonce(data.move_as_ok(), ref_mc, wallets_[item.wallet_idx].source);
    if (nonce.is_ok()) {
      apply_anchored_nonce(item.wallet_idx, nonce.move_as_ok(), kind, td::Time::now());
    } else {
      data = nonce.move_as_error();
    }
  }
  if (data.is_error()) {
    ++stats_.anchor_scan_errors;
    if (item.attempts < options_.max_retries) {
      ++item.attempts;
      scan_items_.push_back(item);
    } else {
      ++scan_failures_;
      LOG(ERROR) << "worker " << worker_id_ << " cannot anchor source "
                 << wallets_[item.wallet_idx].source.to_hex() << " at " << ref_mc.to_str() << ": " << data.error();
    }
  }
  pump_scan();
}

void NativeLoadWorker::finish_scan() {
  auto kind = scan_kind_;
  scan_kind_ = ScanKind::none;
  if (kind == ScanKind::startup) {
    if (scan_failures_) {
      fail(td::Status::Error(PSLICE() << "startup nonce discovery failed for " << scan_failures_ << " sources"));
      return;
    }
    notify_ready();
  } else if (kind == ScanKind::end_boundary) {
    end_snapshot_finished_ = true;
    stats_.end_snapshot_complete = scan_failures_ == 0;
    if (stats_.end_snapshot_complete) {
      stats_.anchored_at_end = count_anchored(true);
      stats_.total_anchored_at_end = count_total_anchored(true);
    }
    next_drain_scan_at_ = td::Time::now();
    request_scan(ScanKind::drain);
  } else if (kind == ScanKind::drain) {
    if (scan_failures_ == 0) {
      stats_.drain_snapshot_complete = true;
      stats_.anchored_after_drain = count_anchored(false);
      stats_.total_anchored_after_drain = count_total_anchored(false);
      stats_.nonce_gaps = stats_.steady_offered >= stats_.anchored_after_drain
                              ? stats_.steady_offered - stats_.anchored_after_drain
                              : 0;
      if (!canonical_cohorts_complete(stats_.anchored_after_drain, stats_.steady_offered,
                                      stats_.total_anchored_after_drain, stats_.offered)) {
        repair_gaps();
        next_drain_scan_at_ = td::Time::now() + options_.finality_poll_seconds;
      } else {
        stats_.drain_to_anchor_seconds = std::max(0.0, td::Time::now() - drain_started_at_);
      }
    } else {
      next_drain_scan_at_ = td::Time::now() + options_.finality_poll_seconds;
    }
  }
  if (pending_scan_ != ScanKind::none && scan_kind_ == ScanKind::none) {
    auto pending = pending_scan_;
    pending_scan_ = ScanKind::none;
    start_scan(pending);
  }
  publish_stats();
  maybe_finish();
}

void NativeLoadWorker::apply_anchored_nonce(std::size_t wallet_idx, td::uint64 nonce, ScanKind kind,
                                            double observed_at) {
  auto& wallet = wallets_[wallet_idx];
  if (kind == ScanKind::startup) {
    wallet.next_nonce = nonce;
    wallet.anchored_nonce = nonce;
    wallet.run_start_nonce = nonce;
    wallet.steady_start_nonce = nonce;
    wallet.steady_end_nonce = nonce;
    wallet.end_snapshot_nonce = nonce;
    return;
  }
  if (!started_) {
    // The exact-baseline account snapshot may be followed by blocks committed
    // while the remaining accounts are still being scanned.  No load has
    // been issued yet, so advancing to the follower-observed nonce is a clean
    // rebase, not an external conflict.
    CHECK(wallet.tasks.empty() && canonical_backlog_ == 0);
    auto rebased_nonce = std::max(wallet.anchored_nonce, nonce);
    wallet.next_nonce = std::max(wallet.next_nonce, rebased_nonce);
    wallet.anchored_nonce = rebased_nonce;
    wallet.run_start_nonce = wallet.next_nonce;
    wallet.steady_start_nonce = wallet.next_nonce;
    wallet.steady_end_nonce = wallet.next_nonce;
    wallet.end_snapshot_nonce = wallet.next_nonce;
    return;
  }
  auto old_anchored = std::min(wallet.anchored_nonce, wallet.next_nonce);
  auto new_anchored = std::min(std::max(wallet.anchored_nonce, nonce), wallet.next_nonce);
  auto newly_anchored = new_anchored - old_anchored;
  canonical_backlog_ = newly_anchored <= canonical_backlog_ ? canonical_backlog_ - newly_anchored : 0;
  if (nonce > wallet.next_nonce) {
    ++stats_.external_nonce_conflicts;
    if (options_.canonical_block_follower) {
      ++stats_.canonical_hash_conflicts;
    }
    disable_wallet_for_conflict(wallet_idx, "canonical nonce advanced beyond this run's issued window");
  }
  wallet.anchored_nonce = std::max(wallet.anchored_nonce, nonce);
  if (kind == ScanKind::end_boundary) {
    wallet.end_snapshot_nonce = nonce;
  }
  while (!wallet.samples.empty() && wallet.samples.front().nonce < nonce) {
    stats_.sampled_anchor_latency.observe_seconds(observed_at - wallet.samples.front().issued_at);
    wallet.samples.pop_front();
  }
  if (!options_.canonical_block_follower) {
    while (!wallet.tasks.empty() && wallet.tasks.begin()->first < nonce) {
      accept_task(wallet.tasks.begin()->second, TaskResolution::proof_observed);
    }
    while (!wallet.admitted_tasks.empty() && wallet.admitted_tasks.begin()->first < nonce) {
      wallet.admitted_tasks.erase(wallet.admitted_tasks.begin());
    }
  }
  enqueue_available_wallet(wallet_idx);
  update_backpressure_state(observed_at);
}

void NativeLoadWorker::observe_canonical_transfers(
    std::vector<CanonicalTransferObservation> observations) {
  auto now = td::Time::now();
  std::map<std::size_t, td::uint64> next_nonces;
  for (const auto& observation : observations) {
    if (observation.wallet_idx >= wallets_.size() ||
        observation.nonce == std::numeric_limits<td::uint64>::max()) {
      continue;
    }
    auto next_nonce = observation.nonce + 1;
    auto [next_it, inserted] = next_nonces.emplace(observation.wallet_idx, next_nonce);
    if (!inserted) {
      next_it->second = std::max(next_it->second, next_nonce);
    }
    auto& wallet = wallets_[observation.wallet_idx];
    if (!started_) {
      continue;
    }
    // A follower never intentionally replays already-followed blocks. Ignore
    // an old observation defensively instead of turning it into a false
    // conflict after its expected hash was retired.
    if (observation.nonce < wallet.anchored_nonce) {
      continue;
    }
    auto expected_it = wallet.expected_hashes.find(observation.nonce);
    bool exact_match = expected_it != wallet.expected_hashes.end() &&
                       std::find(expected_it->second.begin(), expected_it->second.end(),
                                 observation.external_hash) != expected_it->second.end();
    if (exact_match) {
      ++wallet.canonical_total_matched;
      if (nonce_in_half_open_cohort(observation.nonce, wallet.steady_start_nonce,
                                    wallet.steady_end_nonce)) {
        ++wallet.canonical_steady_matched;
      }
      ++stats_.canonical_hash_matched;
      CHECK(wallet.steady_end_nonce >= wallet.steady_start_nonce &&
            wallet.next_nonce >= wallet.run_start_nonce);
      CHECK(wallet.canonical_steady_matched <=
            wallet.steady_end_nonce - wallet.steady_start_nonce);
      CHECK(wallet.canonical_total_matched <= wallet.next_nonce - wallet.run_start_nonce);
      wallet.expected_hashes.erase(expected_it);
      wallet.admitted_tasks.erase(observation.nonce);
      auto task_it = wallet.tasks.find(observation.nonce);
      if (task_it != wallet.tasks.end()) {
        accept_task(task_it->second, TaskResolution::proof_observed);
      }
    } else if (observation.nonce >= wallet.run_start_nonce) {
      ++stats_.canonical_hash_conflicts;
      ++stats_.external_nonce_conflicts;
      disable_wallet_for_conflict(observation.wallet_idx,
                                  "canonical source/nonce belongs to an external or stale-run transfer");
    }
  }
  for (const auto& [wallet_idx, next_nonce] : next_nonces) {
    apply_anchored_nonce(wallet_idx, next_nonce, ScanKind::sample, now);
  }
  pump();
  publish_stats();
  maybe_finish();
}

void NativeLoadWorker::canonical_checkpoint(bool may_capture_measure_end) {
  if (!sending_done_ || finished_) {
    return;
  }
  auto now = td::Time::now();
  if (!end_snapshot_finished_ && may_capture_measure_end) {
    for (auto& wallet : wallets_) {
      wallet.end_snapshot_nonce = wallet.anchored_nonce;
      wallet.end_snapshot_total_matched = wallet.canonical_total_matched;
      wallet.end_snapshot_steady_matched = wallet.canonical_steady_matched;
    }
    end_snapshot_finished_ = true;
    stats_.end_snapshot_complete = true;
    stats_.anchored_at_end = count_anchored(true);
    stats_.total_anchored_at_end = count_total_anchored(true);
  }
  stats_.drain_snapshot_complete = true;
  stats_.anchored_after_drain = count_anchored(false);
  stats_.total_anchored_after_drain = count_total_anchored(false);
  stats_.nonce_gaps = stats_.steady_offered >= stats_.anchored_after_drain
                          ? stats_.steady_offered - stats_.anchored_after_drain
                          : 0;
  if (canonical_cohorts_complete(stats_.anchored_after_drain, stats_.steady_offered,
                                 stats_.total_anchored_after_drain, stats_.offered)) {
    stats_.drain_to_anchor_seconds = std::max(0.0, now - drain_started_at_);
  } else if (now >= next_drain_scan_at_) {
    repair_gaps();
    next_drain_scan_at_ = now + options_.repair_cooldown_seconds;
  }
  publish_stats();
  maybe_finish();
}

void NativeLoadWorker::finalize_after_canonical_poll() {
  if (!finished_ || !options_.canonical_block_follower) {
    fail(td::Status::Error("invalid final canonical snapshot state"));
    return;
  }
  auto now = td::Time::now();
  stats_.drain_snapshot_complete = true;
  stats_.anchored_after_drain = count_anchored(false);
  stats_.total_anchored_after_drain = count_total_anchored(false);
  stats_.nonce_gaps = stats_.steady_offered >= stats_.anchored_after_drain
                          ? stats_.steady_offered - stats_.anchored_after_drain
                          : 0;
  auto settled = stats_.end_snapshot_complete && active_tasks_ == 0 && inflight_ == 0 && signing_ == 0 &&
                 canonical_cohorts_complete(stats_.anchored_after_drain, stats_.steady_offered,
                                            stats_.total_anchored_after_drain, stats_.offered);
  stats_.drain_timed_out = !settled;
  if (settled) {
    stats_.drain_to_anchor_seconds = std::max(0.0, now - drain_started_at_);
  }
  refresh_stats();
  td::actor::send_closure(coordinator_, &NativeLoadCoordinator::worker_finalized,
                          worker_id_, stats_);
  clients_.clear();
  signers_.clear();
  stop();
}

void NativeLoadWorker::repair_gaps() {
  auto now = td::Time::now();
  if (now >= drain_deadline_) {
    return;
  }
  for (std::size_t i = 0; i < wallets_.size(); ++i) {
    if (active_tasks_ >= options_.max_inflight) {
      break;
    }
    auto& wallet = wallets_[i];
    if (!wallet.disabled && !wallet.tasks.count(wallet.anchored_nonce) &&
        wallet.anchored_nonce < wallet.next_nonce) {
      if (wallet.last_repair_nonce == wallet.anchored_nonce &&
          now - wallet.last_repair_at < options_.repair_cooldown_seconds) {
        ++stats_.repair_suppressed;
        continue;
      }
      invalidate_available_wallet(i);
      auto admitted_it = wallet.admitted_tasks.find(wallet.anchored_nonce);
      if (admitted_it != wallet.admitted_tasks.end()) {
        auto task = admitted_it->second;
        CHECK(task && task->state == TaskState::resolved && !task->boc.empty() &&
              task->transfer.nonce == wallet.anchored_nonce &&
              wallet.expected_hashes.count(wallet.anchored_nonce));
        task->state = TaskState::ready;
        task->repair = true;
        task->attempts = 0;
        task->retry_exhaustion_counted = false;
        task->ever_submitted = false;
        task->first_issued_at = now;
        wallet.tasks.emplace(wallet.anchored_nonce, task);
        ++active_tasks_;
        ++stats_.repair_offered;
        wallet.last_repair_nonce = wallet.anchored_nonce;
        wallet.last_repair_at = now;
        ready_tasks_.push_back(std::move(task));
      } else {
        // No admission was ever observed for this nonce, so there is no known
        // pending hash to preserve and a freshly signed repair is appropriate.
        create_transfer(i, wallet.anchored_nonce, false, true);
      }
    }
  }
  pump();
}

td::uint64 NativeLoadWorker::count_anchored(bool end_snapshot) const {
  td::uint64 result = 0;
  for (const auto& wallet : wallets_) {
    if (options_.canonical_block_follower) {
      result += end_snapshot ? wallet.end_snapshot_steady_matched
                             : wallet.canonical_steady_matched;
      continue;
    }
    auto nonce = end_snapshot ? wallet.end_snapshot_nonce : wallet.anchored_nonce;
    auto lower = wallet.steady_start_nonce;
    auto upper = wallet.steady_end_nonce;
    if (nonce > lower) {
      result += std::min(nonce, upper) - lower;
    }
  }
  return result;
}

td::uint64 NativeLoadWorker::count_total_anchored(bool end_snapshot) const {
  td::uint64 result = 0;
  for (const auto& wallet : wallets_) {
    if (options_.canonical_block_follower) {
      result += end_snapshot ? wallet.end_snapshot_total_matched
                             : wallet.canonical_total_matched;
      continue;
    }
    auto nonce = end_snapshot ? wallet.end_snapshot_nonce : wallet.anchored_nonce;
    if (nonce > wallet.run_start_nonce) {
      result += std::min(nonce, wallet.next_nonce) - wallet.run_start_nonce;
    }
  }
  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_WARNING);
  td::set_default_failure_signal_handler().ensure();
  td::set_signal_handler(td::SignalType::Quit, request_stop).ensure();
  Options options;
  td::OptionParser parser;
  parser.set_description("Persistent high-rate native-transfer load generator");
  parser.add_option('h', "help", "show this help", [&] {
    char buffer[32768];
    td::StringBuilder builder(td::MutableSlice{buffer, sizeof(buffer)});
    builder << parser;
    std::cout << builder.as_cslice().c_str();
    std::exit(0);
  });
  parser.add_option('C', "global-config", "path to global.config.json",
                    [&](td::Slice value) { options.global_config = value.str(); });
  parser.add_option('W', "wallet-dir", "directory containing source-N.{pk,pub} and dest-N.pub",
                    [&](td::Slice value) { options.wallet_dir = value.str(); });
  parser.add_option(0, "source-offset", "first source/destination file index",
                    [&](td::Slice value) { options.source_offset = td::to_integer<td::uint32>(value); });
  parser.add_checked_option('s', "sources", "number of source/destination pairs", [&](td::Slice value) {
    options.sources = td::to_integer<td::uint32>(value);
    return options.sources ? td::Status::OK() : td::Status::Error("sources must be positive");
  });
  parser.add_checked_option('c', "connections", "persistent ADNL/TCP connections", [&](td::Slice value) {
    options.connections = td::to_integer<td::uint32>(value);
    return options.connections && options.connections <= 256
               ? td::Status::OK()
               : td::Status::Error("connections must be 1..256");
  });
  parser.add_checked_option('S', "signers", "parallel in-memory signing workers", [&](td::Slice value) {
    options.signers = td::to_integer<td::uint32>(value);
    return options.signers && options.signers <= 256 ? td::Status::OK()
                                                     : td::Status::Error("signers must be 1..256");
  });
  parser.add_checked_option(0, "workers", "independent wallet/client coordinator actors", [&](td::Slice value) {
    options.workers = td::to_integer<td::uint32>(value);
    return options.workers && options.workers <= 256 ? td::Status::OK()
                                                     : td::Status::Error("workers must be 1..256");
  });
  parser.add_checked_option('i', "inflight", "hard maximum outstanding work", [&](td::Slice value) {
    options.max_inflight = td::to_integer<td::uint32>(value);
    return options.max_inflight ? td::Status::OK() : td::Status::Error("inflight must be positive");
  });
  parser.add_checked_option(0, "submit-batch-size", "messages per liteServer.sendMessageBatch query",
                            [&](td::Slice value) {
                              options.submit_batch_size = td::to_integer<td::uint32>(value);
                              return options.submit_batch_size >= 1 && options.submit_batch_size <= 1024
                                         ? td::Status::OK()
                                         : td::Status::Error("submit-batch-size must be 1..1024");
                            });
  parser.add_option(0, "max-canonical-backlog",
                    "proof-observed global outstanding transfer limit; zero disables",
                    [&](td::Slice value) {
                      options.max_canonical_backlog = td::to_integer<td::uint64>(value);
                    });
  parser.add_checked_option(0, "max-source-canonical-backlog",
                            "proof-observed per-source window; zero uses protocol maximum 4096",
                            [&](td::Slice value) {
                              options.max_source_canonical_backlog = td::to_integer<td::uint32>(value);
                              return options.max_source_canonical_backlog <= max_native_nonce_diff
                                         ? td::Status::OK()
                                         : td::Status::Error(
                                               "max-source-canonical-backlog must be 0..4096");
                            });
  parser.add_checked_option('d', "duration", "measured steady-load duration in seconds", [&](td::Slice value) {
    options.duration_seconds = td::to_integer<td::uint32>(value);
    return options.duration_seconds ? td::Status::OK() : td::Status::Error("duration must be positive");
  });
  parser.add_option(0, "ramp-seconds", "linear 0-to-target ramp before warm-up",
                    [&](td::Slice value) { options.ramp_seconds = td::to_integer<td::uint32>(value); });
  parser.add_option(0, "warmup-seconds", "unmeasured full-rate warm-up",
                    [&](td::Slice value) { options.warmup_seconds = td::to_integer<td::uint32>(value); });
  parser.add_checked_option(0, "drain-timeout", "maximum drain/reconciliation seconds", [&](td::Slice value) {
    options.drain_timeout_seconds = td::to_integer<td::uint32>(value);
    return options.drain_timeout_seconds ? td::Status::OK()
                                         : td::Status::Error("drain timeout must be positive");
  });
  parser.add_checked_option(0, "valid-for-seconds", "validity window assigned to each transfer",
                            [&](td::Slice value) {
                              options.valid_for_seconds = td::to_integer<td::uint32>(value);
                              return options.valid_for_seconds
                                         ? td::Status::OK()
                                         : td::Status::Error("valid-for-seconds must be positive");
                            });
  parser.add_checked_option(0, "target-tps", "total offered TPS; zero means unpaced saturation",
                            [&](td::Slice value) {
                              options.target_tps = td::to_double(value);
                              return std::isfinite(options.target_tps) && options.target_tps >= 0.0
                                         ? td::Status::OK()
                                         : td::Status::Error("target TPS must be finite and non-negative");
                            });
  parser.add_checked_option(0, "max-retries", "exact-payload retries before an unknown-outcome cycle",
                            [&](td::Slice value) {
                              options.max_retries = td::to_integer<td::uint32>(value);
                              return options.max_retries <= 100
                                         ? td::Status::OK()
                                         : td::Status::Error("max-retries must be at most 100");
                            });
  parser.add_checked_option(0, "retry-backoff-ms", "initial retry backoff in milliseconds",
                            [&](td::Slice value) {
                              options.retry_backoff_ms = td::to_integer<td::uint32>(value);
                              return options.retry_backoff_ms
                                         ? td::Status::OK()
                                         : td::Status::Error("retry-backoff-ms must be positive");
                            });
  parser.add_option(0, "auto-nonce", "discover proof-checked canonical source nonces before load",
                    [&] { options.auto_nonce = true; });
  parser.add_option(0, "adaptive-inflight", "enable per-connection AIMD request windows",
                    [&] { options.adaptive_inflight = true; });
  parser.add_checked_option(0, "adaptive-initial-rtt-seconds",
                            "initial AIMD bandwidth-delay-product RTT estimate",
                            [&](td::Slice value) {
                              options.adaptive_initial_rtt_seconds = td::to_double(value);
                              return std::isfinite(options.adaptive_initial_rtt_seconds) &&
                                             options.adaptive_initial_rtt_seconds > 0.0 &&
                                             options.adaptive_initial_rtt_seconds <= 60.0
                                         ? td::Status::OK()
                                         : td::Status::Error(
                                               "adaptive initial RTT must be in (0,60] seconds");
                            });
  parser.add_checked_option(0, "finality-poll-seconds", "sampled account-anchor polling interval",
                            [&](td::Slice value) {
                              options.finality_poll_seconds = td::to_double(value);
                              return std::isfinite(options.finality_poll_seconds) &&
                                             options.finality_poll_seconds > 0.0
                                         ? td::Status::OK()
                                         : td::Status::Error("finality poll interval must be positive");
                            });
  parser.add_checked_option(0, "canonical-poll-seconds", "proof-checked canonical block follower interval",
                            [&](td::Slice value) {
                              options.canonical_poll_seconds = td::to_double(value);
                              return std::isfinite(options.canonical_poll_seconds) &&
                                             options.canonical_poll_seconds > 0.0
                                         ? td::Status::OK()
                                         : td::Status::Error("canonical poll interval must be positive");
                            });
  parser.add_checked_option(0, "repair-cooldown-seconds",
                            "minimum delay before resubmitting the same canonical nonce gap",
                            [&](td::Slice value) {
                              options.repair_cooldown_seconds = td::to_double(value);
                              return std::isfinite(options.repair_cooldown_seconds) &&
                                             options.repair_cooldown_seconds > 0.0
                                         ? td::Status::OK()
                                         : td::Status::Error("repair cooldown must be positive");
                            });
  parser.add_option(0, "no-canonical-block-follower",
                    "disable proof-checked masterchain-anchored basechain block following",
                    [&] { options.canonical_block_follower = false; });
  parser.add_option(0, "finality-sample-sources", "maximum sources used for live anchoring latency samples",
                    [&](td::Slice value) {
                      options.finality_sample_sources = td::to_integer<td::uint32>(value);
                    });
  parser.add_checked_option('a', "amount", "transfer amount in TON", [&](td::Slice value) {
    TRY_RESULT(amount, parse_nanograms(value));
    if (!amount) {
      return td::Status::Error("amount must be positive");
    }
    options.amount = amount;
    return td::Status::OK();
  });
  parser.add_checked_option('f', "fee", "native fee in TON", [&](td::Slice value) {
    TRY_RESULT(fee, parse_nanograms(value));
    options.fee = fee;
    return td::Status::OK();
  });
  parser.add_option('n', "start-nonce", "fallback initial nonce for every source when auto-nonce is disabled",
                    [&](td::Slice value) { options.start_nonce = td::to_integer<td::uint64>(value); });
  parser.add_checked_option('t', "query-timeout", "liteserver query timeout in seconds", [&](td::Slice value) {
    options.query_timeout = td::to_double(value);
    return std::isfinite(options.query_timeout) && options.query_timeout > 0.0
               ? td::Status::OK()
               : td::Status::Error("query timeout must be finite and positive");
  });
  parser.add_checked_option('r', "report-interval", "aggregate JSON metrics interval in seconds",
                            [&](td::Slice value) {
                              options.report_interval = td::to_double(value);
                              return std::isfinite(options.report_interval) && options.report_interval > 0.0
                                         ? td::Status::OK()
                                         : td::Status::Error("report interval must be positive");
                            });
  parser.run(argc, argv).ensure();
  if (options.workers > options.sources || options.workers > options.connections ||
      options.workers > options.signers || options.workers > options.max_inflight) {
    LOG(FATAL) << "workers must not exceed sources, connections, signers, or inflight";
  }
  if (options.max_canonical_backlog && options.max_canonical_backlog < options.workers) {
    LOG(FATAL) << "max-canonical-backlog must be zero or at least the worker count";
  }
  if (options.submit_batch_size > 1 && options.query_timeout < 9.0) {
    LOG(FATAL) << "batched submission requires query-timeout >= 9 seconds; the server owns "
                  "admission work for at most 8 seconds";
  }
  if ((options.max_canonical_backlog || options.max_source_canonical_backlog) &&
      !options.auto_nonce && !options.canonical_block_follower) {
    LOG(FATAL) << "canonical backlog controls require auto-nonce or the canonical block follower";
  }
  if (static_cast<td::uint64>(options.source_offset) + options.sources >
      std::numeric_limits<td::uint32>::max()) {
    LOG(FATAL) << "source offset plus source count overflows uint32";
  }

  vm::init_vm(true).ensure();
  auto scheduler_threads = std::clamp(options.signers + options.workers + 1, 2u, 127u);
  td::actor::Scheduler scheduler({scheduler_threads});
  td::actor::ActorOwn<NativeLoadCoordinator> coordinator;
  scheduler.run_in_context([&] {
    coordinator = td::actor::create_actor<NativeLoadCoordinator>("native-load-coordinator", std::move(options));
    coordinator.release();
  });
  scheduler.run();
  return process_exit_code.load();
}
