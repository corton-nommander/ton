/*
    High-rate native-transfer load generator for a private TON sidechain.

    The generator is deliberately a standalone process/container.  Workers own
    disjoint wallet ranges, signer actors and persistent ADNL/TCP connections.
    A coordinator only aggregates metrics and owns process shutdown.
*/
#include "auto/tl/lite_api.hpp"
#include "auto/tl/ton_api_json.h"
#include "block/block-auto.h"
#include "block/check-proof.h"
#include "block/transaction.h"
#include "crypto/Ed25519.h"
#include "td/actor/actor.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/signals.h"
#include "tl-utils/lite-utils.hpp"
#include "ton/lite-tl.hpp"
#include "vm/boc.h"
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
  double target_tps{0.0};
  double query_timeout{10.0};
  double report_interval{1.0};
  double finality_poll_seconds{10.0};
  bool auto_nonce{false};
  bool adaptive_inflight{false};
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
  td::uint64 retries{0};
  td::uint64 retry_exhausted{0};
  td::uint64 resigned{0};
  td::uint64 repair_offered{0};
  td::uint64 mempool_accepted{0};
  td::uint64 steady_mempool_accepted{0};
  td::uint64 repair_accepted{0};
  td::uint64 accepted_inferred{0};
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
  td::uint64 inflight{0};
  td::uint64 signing{0};
  td::uint64 ready{0};
  td::uint64 retry_wait{0};
  td::uint64 active_sources{0};
  double congestion_window{0.0};
  double pacing_tokens{0.0};
  double measure_elapsed_seconds{0.0};
  double drain_to_anchor_seconds{0.0};
  bool steady_started{false};
  bool sending_done{false};
  bool end_snapshot_complete{false};
  bool drain_snapshot_complete{false};
  bool drain_timed_out{false};
  bool interrupted{false};
  LatencyHistogram signing_latency;
  LatencyHistogram request_latency;
  LatencyHistogram sampled_anchor_latency;
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

 private:
  class Signer final : public td::actor::Actor {
   public:
    void sign(block::NativeTransfer transfer,
              std::shared_ptr<const td::Ed25519::PreparedPrivateKey> private_key,
              td::Promise<td::BufferSlice> promise) {
      auto signature = td::Ed25519::PrivateKey::sign(*private_key, transfer.signing_payload());
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
      auto boc = vm::std_boc_serialize(std::move(root));
      if (boc.is_error()) {
        promise.set_error(boc.move_as_error());
      } else {
        promise.set_value(boc.move_as_ok());
      }
    }
  };

  enum class TaskState { signing, ready, inflight, retry_wait, resolved };

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
    bool retry_exhaustion_counted{false};
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
    td::uint64 steady_start_nonce{0};
    td::uint64 steady_end_nonce{0};
    td::uint64 end_snapshot_nonce{0};
    bool sampled{false};
    bool disabled{false};
    bool available_queued{false};
    td::uint64 available_generation{0};
    std::shared_ptr<TransferTask> active;
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

  void start_up() override;
  td::Status initialize();
  void notify_ready();
  void fail(td::Status error);
  void alarm() override;
  void update_phase(double now);
  void update_tokens(double now);
  bool is_measure_phase(double now) const;
  bool can_issue(double now) const;
  void pump();
  td::optional<std::size_t> find_available_wallet();
  void enqueue_available_wallet(std::size_t wallet_idx);
  void invalidate_available_wallet(std::size_t wallet_idx);
  void create_transfer(std::size_t wallet_idx, td::uint64 nonce, bool measured, bool repair);
  void sign_task(std::shared_ptr<TransferTask> task, bool resign);
  void on_signed(std::shared_ptr<TransferTask> task, td::Result<td::BufferSlice> message);
  td::optional<std::size_t> select_client() const;
  void dispatch_ready();
  void send_task(std::shared_ptr<TransferTask> task, std::size_t client_idx);
  void on_result(std::shared_ptr<TransferTask> task, std::size_t client_idx,
                 td::Result<td::BufferSlice> result);
  void handle_task_error(std::shared_ptr<TransferTask> task, std::size_t client_idx, td::Status error,
                         ErrorOrigin origin);
  void schedule_retry(std::shared_ptr<TransferTask> task, double delay_seconds);
  void accept_task(std::shared_ptr<TransferTask> task, bool inferred);
  void reject_task(std::shared_ptr<TransferTask> task, td::Slice reason);
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
};

class NativeLoadCoordinator final : public td::actor::Actor {
 public:
  explicit NativeLoadCoordinator(Options options) : options_(std::move(options)) {
  }

  void worker_ready(td::uint32 worker_id) {
    if (failed_ || worker_id >= worker_ready_.size() || worker_ready_[worker_id]) {
      return;
    }
    worker_ready_[worker_id] = true;
    ++ready_count_;
    if (ready_count_ == workers_.size()) {
      start_at_ = td::Time::now() + 0.2;
      started_ = true;
      for (auto& worker : workers_) {
        td::actor::send_closure(worker, &NativeLoadWorker::begin, start_at_);
      }
      LOG(WARNING) << "native load generator ready: workers=" << workers_.size()
                   << " sources=" << options_.sources << " connections=" << options_.connections
                   << " signers=" << options_.signers << " max_inflight=" << options_.max_inflight
                   << " target_tps=" << options_.target_tps << " ramp=" << options_.ramp_seconds
                   << "s warmup=" << options_.warmup_seconds << "s duration=" << options_.duration_seconds
                   << "s drain_timeout=" << options_.drain_timeout_seconds << "s";
      last_report_at_ = start_at_;
      alarm_timestamp() = td::Timestamp::in(std::max(0.001, start_at_ - td::Time::now()));
    }
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
      report(true);
      auto total = aggregate();
      if (total.drain_timed_out) {
        process_exit_code.store(2);
        LOG(ERROR) << "native load generator stopped with an unsettled drain timeout";
      } else {
        LOG(WARNING) << "native load generator stopped cleanly";
      }
      workers_.clear();
      td::actor::SchedulerContext::get().stop();
      stop();
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
  Options options_;
  std::vector<td::actor::ActorOwn<NativeLoadWorker>> workers_;
  std::vector<bool> worker_ready_;
  std::vector<bool> worker_done_;
  std::vector<WorkerStats> snapshots_;
  WorkerStats previous_;
  td::uint32 ready_count_{0};
  td::uint32 done_count_{0};
  bool started_{false};
  bool failed_{false};
  bool stop_sent_{false};
  double start_at_{0.0};
  double last_report_at_{0.0};

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
    snapshots_.resize(options_.workers);
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
    // Startup nonce discovery can involve many proof queries.  Poll signals
    // while it runs so `docker stop` never has to wait for all scans to finish.
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
      ADD_FIELD(retries);
      ADD_FIELD(retry_exhausted);
      ADD_FIELD(resigned);
      ADD_FIELD(repair_offered);
      ADD_FIELD(mempool_accepted);
      ADD_FIELD(steady_mempool_accepted);
      ADD_FIELD(repair_accepted);
      ADD_FIELD(accepted_inferred);
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
      ADD_FIELD(inflight);
      ADD_FIELD(signing);
      ADD_FIELD(ready);
      ADD_FIELD(retry_wait);
      ADD_FIELD(active_sources);
#undef ADD_FIELD
      total.congestion_window += value.congestion_window;
      total.pacing_tokens += value.pacing_tokens;
      total.measure_elapsed_seconds = std::max(total.measure_elapsed_seconds, value.measure_elapsed_seconds);
      total.drain_to_anchor_seconds = std::max(total.drain_to_anchor_seconds, value.drain_to_anchor_seconds);
      total.steady_started = total.steady_started && value.steady_started;
      total.sending_done = total.sending_done && value.sending_done;
      total.end_snapshot_complete = total.end_snapshot_complete && value.end_snapshot_complete;
      total.drain_snapshot_complete = total.drain_snapshot_complete && value.drain_snapshot_complete;
      total.drain_timed_out = total.drain_timed_out || value.drain_timed_out;
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
    auto interval = std::max(0.000001, now - last_report_at_);
    auto rate = [interval](td::uint64 current, td::uint64 previous) {
      return static_cast<double>(current - previous) / interval;
    };
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
              << ",\"retries\":" << total.retries << ",\"retry_exhausted\":" << total.retry_exhausted
              << ",\"resigned\":" << total.resigned << ",\"repair_offered\":" << total.repair_offered
              << ",\"mempool_accepted\":"
              << total.mempool_accepted << ",\"stored\":" << total.mempool_accepted
              << ",\"admitted\":" << total.mempool_accepted << ",\"mempool_accept_tps\":"
              << rate(total.mempool_accepted, previous_.mempool_accepted)
              << ",\"steady_mempool_accepted\":" << total.steady_mempool_accepted
              << ",\"repair_accepted\":" << total.repair_accepted
              << ",\"accepted_inferred\":" << total.accepted_inferred
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
              << ",\"signing\":" << total.signing << ",\"ready\":" << total.ready
              << ",\"retry_wait\":" << total.retry_wait << ",\"active_sources\":" << total.active_sources
              << ",\"interrupted\":" << (total.interrupted ? "true" : "false")
              << ",\"drain_timed_out\":" << (total.drain_timed_out ? "true" : "false")
              << ",\"congestion_window\":" << total.congestion_window << ",\"pacing_tokens\":"
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
              << total.anchor_scan_errors << ",\"canonical_at_measure_end\":";
    if (total.end_snapshot_complete) {
      std::cout << total.anchored_at_end << ",\"canonical_backlog_at_measure_end\":"
                << (total.steady_offered >= total.anchored_at_end ? total.steady_offered - total.anchored_at_end : 0)
                << ",\"canonical_tps_at_measure_end\":"
                << static_cast<double>(total.anchored_at_end) /
                       std::max(0.000001, total.measure_elapsed_seconds);
    } else {
      std::cout << "null,\"canonical_backlog_at_measure_end\":null,\"canonical_tps_at_measure_end\":null";
    }
    std::cout << ",\"canonical_after_drain\":";
    if (total.drain_snapshot_complete) {
      std::cout << total.anchored_after_drain << ",\"canonical_backlog_after_drain\":"
                << (total.steady_offered >= total.anchored_after_drain
                        ? total.steady_offered - total.anchored_after_drain
                        : 0)
                << ",\"drain_to_anchor_s\":" << total.drain_to_anchor_seconds;
    } else {
      std::cout << "null,\"canonical_backlog_after_drain\":null,\"drain_to_anchor_s\":null";
    }
    std::cout << ",\"finalized\":null,\"finalized_semantics\":\"not_independently_observed\""
              << ",\"canonical_semantics\":\"proof_checked_masterchain_anchored_account_nonces\"}"
              << std::endl;
    previous_ = total;
  }
};

void NativeLoadWorker::start_up() {
  auto status = initialize();
  if (status.is_error()) {
    fail(status.move_as_error());
    return;
  }
  if (options_.auto_nonce) {
    start_scan(ScanKind::startup);
  } else {
    notify_ready();
  }
}

td::Status NativeLoadWorker::initialize() {
  clients_.reserve(options_.connections);
  td::uint32 per_client = options_.max_inflight / options_.connections;
  td::uint32 per_client_extra = options_.max_inflight % options_.connections;
  // Start near a 250 ms bandwidth-delay product.  The previous 50 ms guess
  // made a 100k TPS test spend minutes growing its application-level window
  // even on an otherwise idle validator.
  double guessed_window = options_.target_tps > 0.0
                              ? std::max(16.0, options_.target_tps * 0.25 / options_.connections)
                              : 256.0;
  for (td::uint32 i = 0; i < options_.connections; ++i) {
    ClientSlot slot;
    slot.actor = liteclient::ExtClient::create(servers_, nullptr);
    slot.hard_limit = per_client + (i < per_client_extra ? 1u : 0u);
    slot.cwnd = options_.adaptive_inflight ? std::min<double>(slot.hard_limit, guessed_window)
                                           : static_cast<double>(slot.hard_limit);
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
    wallet.sampled = i < sample_sources_;
    wallets_.push_back(std::move(wallet));
  }
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
    if (!sending_done_ && options_.auto_nonce && scan_kind_ == ScanKind::none && now >= next_sample_at_ &&
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
  return options_.target_tps <= 0.0 || pacing_tokens_ >= 1.0;
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
    if (!wallet.disabled && !wallet.active) {
      return entry.wallet_idx;
    }
  }
  return {};
}

void NativeLoadWorker::enqueue_available_wallet(std::size_t wallet_idx) {
  auto& wallet = wallets_[wallet_idx];
  if (wallet.disabled || wallet.active || wallet.available_queued) {
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
  update_tokens(now);
  while (!retry_tasks_.empty() && retry_tasks_.begin()->first <= now) {
    auto task = retry_tasks_.begin()->second;
    retry_tasks_.erase(retry_tasks_.begin());
    if (task->state != TaskState::retry_wait || wallets_[task->wallet_idx].active != task) {
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
    bool measured = is_measure_phase(now);
    auto nonce = wallets_[wallet_idx.value()].next_nonce++;
    create_transfer(wallet_idx.value(), nonce, measured, false);
    if (options_.target_tps > 0.0) {
      pacing_tokens_ -= 1.0;
    }
    now = td::Time::now();
    update_tokens(now);
  }
  dispatch_ready();
}

void NativeLoadWorker::create_transfer(std::size_t wallet_idx, td::uint64 nonce, bool measured, bool repair) {
  auto& wallet = wallets_[wallet_idx];
  CHECK(!wallet.disabled && !wallet.active);
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
  wallet.active = task;
  ++active_tasks_;
  if (repair) {
    ++stats_.repair_offered;
  } else {
    ++stats_.offered;
    if (measured) {
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
    ++stats_.resigned;
  }
  auto promise = td::PromiseCreator::lambda(
      [self = actor_id(this), task](td::Result<td::BufferSlice> message) mutable {
        td::actor::send_closure(self, &NativeLoadWorker::on_signed, std::move(task), std::move(message));
      });
  auto& signer = signers_[signer_cursor_++ % signers_.size()];
  td::actor::send_closure(signer, &Signer::sign, task->transfer,
                          wallets_[task->wallet_idx].private_key, std::move(promise));
  ++signing_;
}

void NativeLoadWorker::on_signed(std::shared_ptr<TransferTask> task, td::Result<td::BufferSlice> message) {
  CHECK(signing_ > 0);
  --signing_;
  ++stats_.sign_operations;
  stats_.signing_latency.observe_seconds(td::Time::now() - task->sign_started_at);
  auto& wallet = wallets_[task->wallet_idx];
  if (wallet.active != task) {
    pump();
    return;
  }
  if (message.is_error()) {
    ++stats_.sign_errors;
    LOG(ERROR) << "worker " << worker_id_ << " signing failed for nonce " << task->transfer.nonce << ": "
               << message.error();
    schedule_retry(std::move(task), std::max(0.001, options_.retry_backoff_ms / 1000.0));
  } else {
    task->boc = message.move_as_ok();
    task->state = TaskState::ready;
    ready_tasks_.push_back(std::move(task));
  }
  pump();
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

void NativeLoadWorker::dispatch_ready() {
  while (!ready_tasks_.empty() && inflight_ < options_.max_inflight) {
    auto client_idx = select_client();
    if (!client_idx) {
      break;
    }
    auto task = ready_tasks_.front();
    ready_tasks_.pop_front();
    if (task->state != TaskState::ready || wallets_[task->wallet_idx].active != task) {
      continue;
    }
    send_task(std::move(task), client_idx.value());
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
  ++inflight_;
  ++clients_[client_idx].inflight;
  td::actor::send_closure(clients_[client_idx].actor, &liteclient::ExtClient::send_query, "native-load",
                          envelope_query(std::move(query)), td::Timestamp::in(options_.query_timeout),
                          std::move(promise));
}

void NativeLoadWorker::on_result(std::shared_ptr<TransferTask> task, std::size_t client_idx,
                                 td::Result<td::BufferSlice> result) {
  CHECK(inflight_ > 0 && client_idx < clients_.size() && clients_[client_idx].inflight > 0);
  --inflight_;
  --clients_[client_idx].inflight;
  stats_.request_latency.observe_seconds(td::Time::now() - task->last_sent_at);
  auto& wallet = wallets_[task->wallet_idx];
  if (wallet.active != task) {
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
      accept_task(std::move(task), false);
    } else {
      ++stats_.rejected_other;
      reject_task(std::move(task), "sendMessage returned a non-success status");
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
  bool balance = contains("insufficient") || contains("balance");
  bool invalid = contains("signature") || contains("wrong source") || contains("must be balance-only") ||
                 contains("overflow");
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
  if (duplicate || too_old) {
    accept_task(std::move(task), true);
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

void NativeLoadWorker::accept_task(std::shared_ptr<TransferTask> task, bool inferred) {
  auto& wallet = wallets_[task->wallet_idx];
  if (wallet.active != task) {
    return;
  }
  if (inferred) {
    ++stats_.accepted_inferred;
  } else {
    ++stats_.mempool_accepted;
    if (task->repair) {
      ++stats_.repair_accepted;
    } else if (task->measured) {
      ++stats_.steady_mempool_accepted;
    }
  }
  task->state = TaskState::resolved;
  CHECK(active_tasks_ > 0);
  --active_tasks_;
  wallet.active.reset();
  enqueue_available_wallet(task->wallet_idx);
}

void NativeLoadWorker::reject_task(std::shared_ptr<TransferTask> task, td::Slice reason) {
  auto& wallet = wallets_[task->wallet_idx];
  if (wallet.active != task) {
    return;
  }
  ++stats_.rejected;
  wallet.disabled = true;
  task->state = TaskState::resolved;
  CHECK(active_tasks_ > 0);
  --active_tasks_;
  wallet.active.reset();
  LOG(ERROR) << "worker " << worker_id_ << " permanently rejected source " << wallet.source.to_hex()
             << " nonce " << task->transfer.nonce << ": " << reason;
}

void NativeLoadWorker::begin_drain(double now) {
  if (sending_done_) {
    return;
  }
  sending_done_ = true;
  stats_.sending_done = true;
  drain_started_at_ = now;
  drain_deadline_ = now + options_.drain_timeout_seconds;
  next_drain_scan_at_ = now;
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
  if (options_.auto_nonce) {
    request_scan(ScanKind::end_boundary);
  }
}

void NativeLoadWorker::maybe_finish() {
  if (!sending_done_ || finished_) {
    return;
  }
  auto now = td::Time::now();
  if (now >= drain_deadline_) {
    if (options_.auto_nonce && !stats_.drain_snapshot_complete && !final_scan_grace_used_) {
      final_scan_grace_used_ = true;
      if (scan_kind_ == ScanKind::none) {
        request_scan(ScanKind::drain);
      }
      // Give the final snapshot at most one query timeout of grace.  This is a
      // one-shot extension; an unfinalized run cannot extend itself forever.
      drain_deadline_ = now + options_.query_timeout;
      return;
    }
    stats_.drain_timed_out = active_tasks_ != 0 ||
                             (options_.auto_nonce &&
                              (!stats_.drain_snapshot_complete ||
                               stats_.anchored_after_drain < stats_.steady_offered));
    finish();
    return;
  }
  if (!options_.auto_nonce) {
    if (!active_tasks_) {
      finish();
    }
    return;
  }
  if (scan_kind_ == ScanKind::none && now >= next_drain_scan_at_) {
    if (!end_snapshot_finished_) {
      request_scan(ScanKind::end_boundary);
    } else if (!stats_.drain_snapshot_complete || stats_.anchored_after_drain < stats_.steady_offered) {
      request_scan(ScanKind::drain);
    }
  }
  if (active_tasks_) {
    return;
  }
  if (stats_.drain_snapshot_complete && stats_.anchored_after_drain >= stats_.steady_offered) {
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
  clients_.clear();
  signers_.clear();
  stop();
}

void NativeLoadWorker::refresh_stats() {
  stats_.inflight = inflight_;
  stats_.signing = signing_;
  stats_.ready = 0;
  stats_.retry_wait = 0;
  stats_.active_sources = 0;
  stats_.congestion_window = 0.0;
  for (const auto& wallet : wallets_) {
    if (!wallet.active) {
      continue;
    }
    ++stats_.active_sources;
    if (wallet.active->state == TaskState::ready) {
      ++stats_.ready;
    } else if (wallet.active->state == TaskState::retry_wait) {
      ++stats_.retry_wait;
    }
  }
  for (const auto& client : clients_) {
    stats_.congestion_window += client.cwnd;
  }
  stats_.pacing_tokens = pacing_tokens_;
  stats_.interrupted = interrupted_;
  if (started_) {
    auto measure_begin = start_at_ + options_.ramp_seconds + options_.warmup_seconds;
    auto configured_end = measure_begin + options_.duration_seconds;
    auto observed_end = sending_done_ ? drain_started_at_ : td::Time::now();
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
  if (kind == ScanKind::sample) {
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
    }
    next_drain_scan_at_ = td::Time::now();
    request_scan(ScanKind::drain);
  } else if (kind == ScanKind::drain) {
    if (scan_failures_ == 0) {
      stats_.drain_snapshot_complete = true;
      stats_.anchored_after_drain = count_anchored(false);
      stats_.nonce_gaps = stats_.steady_offered >= stats_.anchored_after_drain
                              ? stats_.steady_offered - stats_.anchored_after_drain
                              : 0;
      if (stats_.anchored_after_drain < stats_.steady_offered) {
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
    wallet.steady_start_nonce = nonce;
    wallet.steady_end_nonce = nonce;
    wallet.end_snapshot_nonce = nonce;
    return;
  }
  if (nonce > wallet.next_nonce) {
    ++stats_.external_nonce_conflicts;
    wallet.disabled = true;
    invalidate_available_wallet(wallet_idx);
  }
  wallet.anchored_nonce = std::max(wallet.anchored_nonce, nonce);
  if (kind == ScanKind::end_boundary) {
    wallet.end_snapshot_nonce = nonce;
  }
  while (!wallet.samples.empty() && wallet.samples.front().nonce < nonce) {
    stats_.sampled_anchor_latency.observe_seconds(observed_at - wallet.samples.front().issued_at);
    wallet.samples.pop_front();
  }
  if (wallet.active && wallet.active->transfer.nonce < nonce) {
    accept_task(wallet.active, true);
  }
}

void NativeLoadWorker::repair_gaps() {
  if (td::Time::now() >= drain_deadline_) {
    return;
  }
  for (std::size_t i = 0; i < wallets_.size(); ++i) {
    if (active_tasks_ >= options_.max_inflight) {
      break;
    }
    auto& wallet = wallets_[i];
    if (!wallet.disabled && !wallet.active && wallet.anchored_nonce < wallet.next_nonce) {
      invalidate_available_wallet(i);
      create_transfer(i, wallet.anchored_nonce, false, true);
    }
  }
  pump();
}

td::uint64 NativeLoadWorker::count_anchored(bool end_snapshot) const {
  td::uint64 result = 0;
  for (const auto& wallet : wallets_) {
    auto nonce = end_snapshot ? wallet.end_snapshot_nonce : wallet.anchored_nonce;
    auto lower = wallet.steady_start_nonce;
    auto upper = wallet.steady_end_nonce;
    if (nonce > lower) {
      result += std::min(nonce, upper) - lower;
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
  parser.add_checked_option(0, "finality-poll-seconds", "sampled account-anchor polling interval",
                            [&](td::Slice value) {
                              options.finality_poll_seconds = td::to_double(value);
                              return std::isfinite(options.finality_poll_seconds) &&
                                             options.finality_poll_seconds > 0.0
                                         ? td::Status::OK()
                                         : td::Status::Error("finality poll interval must be positive");
                            });
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
