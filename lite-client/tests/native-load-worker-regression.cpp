// Appended to a temporary copy of native-load-generator.cpp by the companion
// Python runner. These tests execute the production worker methods; synthetic
// signed bytes and canonical observations test identity/accounting, not crypto
// verification or remote chain throughput. No network requests are dispatched.
// The real coordinator sink intentionally has /dev/null as its config, so any
// incidental startup fails before an endpoint can be created (expected stderr).

namespace {
struct RecoveryFixture {
  Options options;
  std::unique_ptr<NativeLoadWorker> worker;
  std::vector<std::shared_ptr<NativeLoadWorker::TransferTask>> parents;
  std::vector<std::string> bytes;
  std::vector<ton::Bits256> hashes;
  ton::UnixTime valid_until;

  explicit RecoveryFixture(std::size_t parent_count, td::actor::ActorId<NativeLoadCoordinator> coordinator = {}) {
    options.native_signed_runs.requested = true;
    options.adaptive_inflight = true;
    options.max_inflight = 256;
    options.adaptive_max_cwnd = 256;
    options.max_source_canonical_backlog = 128;
    options.max_retries = 8;
    options.retry_backoff_ms = 100;
    options.retry_horizon_seconds = 90;
    options.drain_timeout_seconds = 180;
    options.global_config = "/dev/null";
    worker = std::make_unique<NativeLoadWorker>(0, options, std::vector<liteclient::LiteServerConfig>{}, 0, coordinator);
    worker->native_signed_run_quantum_ = 16;
    worker->wallets_.resize(1);
    worker->ready_wallets_.reset(1);
    worker->clients_.resize(1);
    auto& client = worker->clients_[0];
    client.hard_limit = 256;
    client.cwnd_limit = 256;
    client.cwnd = 256;
    worker->active_tasks_ = parent_count * 16;
    worker->canonical_backlog_ = parent_count * 16;
    worker->stats_.offered = worker->stats_.steady_offered = parent_count * 16;
    auto& wallet = worker->wallets_[0];
    wallet.source.as_slice().fill(0);
    wallet.run_start_nonce = wallet.anchored_nonce = wallet.steady_start_nonce = 100;
    wallet.next_nonce = wallet.steady_end_nonce = 100 + parent_count * 16;
    valid_until = static_cast<ton::UnixTime>(td::Clocks::system() + 7200);
    for (std::size_t i = 0; i < parent_count; ++i) {
      auto task = std::make_shared<NativeLoadWorker::TransferTask>();
      task->wallet_idx = 0;
      task->signed_run = true;
      task->run.first_nonce = 100 + i * 16;
      task->run.valid_until = valid_until;
      task->run.outputs.resize(16);
      task->run.signature = std::string(64, static_cast<char>('a' + i));
      bytes.push_back("immutable NTRN fixture parent " + std::to_string(i));
      task->boc = td::BufferSlice(bytes.back());
      task->proof_observed.assign(16, false);
      task->state = NativeLoadWorker::TaskState::retry_wait;
      task->attempts = 1;
      task->ever_submitted = true;
      task->measured = true;
      ton::Bits256 hash;
      hash.as_slice().fill(static_cast<char>('A' + i));
      hashes.push_back(hash);
      wallet.tasks.emplace(task->first_nonce(), task);
      worker->remember_expected_hash(task, hash);
      parents.push_back(task);
    }
  }
  void check_identity() const {
    for (std::size_t i = 0; i < parents.size(); ++i) {
      const auto& task = parents[i];
      CHECK(task->boc.as_slice().str() == bytes[i]);
      CHECK(task->valid_until() == valid_until && task->first_nonce() == 100 + 16*i);
      CHECK(task->logical_count() == 16 && task->run.signature == std::string(64, static_cast<char>('a'+i)));
      for (td::uint64 nonce = task->first_nonce(); nonce < task->first_nonce()+16; ++nonce) {
        const auto& known = worker->wallets_[0].expected_hashes.at(nonce);
        CHECK(known.size() == 1 && known[0] == hashes[i]);
      }
    }
    CHECK(worker->stats_.resigned == 0 && worker->signing_ == 0);
  }
  void fail_parent(std::size_t i, const std::string& diagnostic) {
    worker->handle_task_error(parents[i], 0,
        td::Status::Error(ton::ErrorCode::notready, diagnostic), NativeLoadWorker::ErrorOrigin::server);
  }
  void trigger_head_horizon() {
    parents[0]->first_retry_at = td::Time::now() - 185;
    parents[0]->source_head_retry_at = td::Time::now() - 185;
    fail_parent(0, "native transfer run admission snapshot changed; retry");
    auto& wallet = worker->wallets_[0];
    CHECK(wallet.retry_quarantined && !wallet.disabled);
    CHECK(wallet.tasks.size() == parents.size());
    CHECK(worker->active_tasks_ == parents.size()*16);
    CHECK(worker->stats_.retry_exhausted == 1 && worker->stats_.retry_exhausted_sources == 1);
    CHECK(parents[0]->state == NativeLoadWorker::TaskState::retry_wait);
    worker->enqueue_available_wallet(0);
    CHECK(!worker->find_available_wallet());
    check_identity();
  }
};

void exercise_deadline_callback(bool batch) {
  Options options;
  options.native_signed_runs.requested = true;
  options.adaptive_inflight = true;
  options.max_inflight = 64;
  options.adaptive_max_cwnd = 64;
  options.retry_horizon_seconds = 90;
  NativeLoadWorker worker(0, options, {}, 0, {});
  worker.native_signed_run_quantum_ = 16;
  const std::size_t count = batch ? 2 : 1;
  worker.wallets_.resize(count);
  worker.ready_wallets_.reset(count);
  worker.clients_.resize(1);
  auto& client = worker.clients_[0];
  client.hard_limit = 64;
  client.cwnd_limit = 64;
  client.cwnd = 64;
  client.inflight = static_cast<td::uint32>(count * 16);
  client.admission_queries_inflight = 1;
  worker.inflight_ = count * 16;
  worker.active_tasks_ = count * 16;
  const auto valid_until = static_cast<ton::UnixTime>(td::Clocks::system() + 7200);
  const std::string original_boc = "exact source-signed NTRN parent fixture bytes";
  const std::string original_signature(64, 's');
  ton::Bits256 original_hash;
  original_hash.as_slice().fill('h');
  std::vector<std::shared_ptr<NativeLoadWorker::TransferTask>> tasks;
  for (std::size_t i = 0; i < count; ++i) {
    auto& wallet = worker.wallets_[i];
    wallet.run_start_nonce = wallet.anchored_nonce = 100;
    wallet.next_nonce = 116;
    auto task = std::make_shared<NativeLoadWorker::TransferTask>();
    task->wallet_idx = i;
    task->signed_run = true;
    task->run.first_nonce = 100;
    task->run.valid_until = valid_until;
    task->run.outputs.resize(16);
    task->run.signature = original_signature;
    task->boc = td::BufferSlice(original_boc);
    task->proof_observed.assign(16, false);
    task->state = NativeLoadWorker::TaskState::inflight;
    task->last_sent_at = td::Time::now();
    task->attempts = 1;
    task->ever_submitted = true;
    wallet.tasks.emplace(100, task);
    worker.remember_expected_hash(task, original_hash);
    tasks.push_back(task);
  }
  const std::string diagnostic = "external message admission deadline expired";
  if (batch) {
    std::vector<ton::tl_object_ptr<ton::lite_api::liteServer_sendMsgResult>> results;
    for (std::size_t i = 0; i < count; ++i) {
      results.push_back(ton::create_tl_object<ton::lite_api::liteServer_sendMsgResult>(
          0, ton::ErrorCode::timeout, diagnostic));
    }
    auto response = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_sendMsgStatusBatch>(std::move(results)), true);
    worker.on_batch_result(tasks, 0, std::move(response));
  } else {
    auto response = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_error>(ton::ErrorCode::timeout, diagnostic), true);
    worker.on_result(tasks.front(), 0, std::move(response));
  }
  CHECK(worker.inflight_ == 0 && client.inflight == 0 && client.admission_queries_inflight == 0);
  CHECK(worker.active_tasks_ == count * 16 && worker.signing_ == 0);
  CHECK(worker.stats_.resigned == 0 && worker.stats_.rejected_expired == 0);
  CHECK(worker.stats_.mempool_accepted == 0 && worker.stats_.canonical_hash_matched == 0);
  CHECK(worker.stats_.timeouts == count && worker.stats_.task_errors_by_reason.timeout == count);
  CHECK(worker.stats_.retries == count && worker.stats_.retries_by_reason.timeout == count);
  CHECK(worker.retry_tasks_.size() == count);
  CHECK(client.cwnd == 32);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& task = tasks[i];
    const auto& wallet = worker.wallets_[i];
    CHECK(task->state == NativeLoadWorker::TaskState::retry_wait);
    CHECK(task->retry_reason == TaskErrorReason::timeout);
    CHECK(task->retry_at >= task->first_retry_at && task->first_retry_at >= 0);
    CHECK(task->boc.as_slice().str() == original_boc);
    CHECK(task->run.signature == original_signature && task->valid_until() == valid_until);
    CHECK(task->first_nonce() == 100 && task->logical_count() == 16);
    CHECK(task->ever_submitted && !task->admission_counted && !task->resigned_after_expiry);
    CHECK(wallet.tasks.size() == 1 && wallet.tasks.at(100) == task && !wallet.disabled);
    CHECK(wallet.expected_hashes.size() == 16);
    for (td::uint64 nonce = 100; nonce < 116; ++nonce) {
      const auto& hashes = wallet.expected_hashes.at(nonce);
      CHECK(hashes.size() == 1 && hashes.front() == original_hash);
    }
  }
  std::cout << (batch ? "batch_32_logical" : "single_16_logical")
            << ": PASS exact parent bytes/signature/hash/valid_until retained; timeout retried without admission/proof credit\n";
}

void exercise_bounded_revision_retry() {
  RecoveryFixture fixture(1);
  const std::string race = "native account changed before mempool insertion; retry admission";
  for (unsigned i = 0; i < 12; ++i) {
    fixture.parents[0]->attempts = i + 1;
    const auto before = td::Time::now();
    fixture.fail_parent(0, race);
    const auto after = td::Time::now();
    const double expected = std::min(2.0, 0.25 * static_cast<double>(1u << std::min(i, 4u)));
    CHECK(fixture.parents[0]->retry_at >= before + expected - .001);
    CHECK(fixture.parents[0]->retry_at <= after + expected + .001);
  }
  CHECK(fixture.worker->stats_.snapshot_revision_not_ready_errors == 12);
  CHECK(fixture.worker->stats_.other_not_ready_errors == 0);
  fixture.fail_parent(0, "validator not ready");
  CHECK(fixture.parents[0]->snapshot_revision_retry_attempts == 0);
  CHECK(fixture.worker->stats_.other_not_ready_errors == 1);
  fixture.fail_parent(0, race);
  CHECK(fixture.parents[0]->snapshot_revision_retry_attempts == 1);
  fixture.check_identity();
  std::cout << "bounded_revision_retry: PASS 250/500/1000/2000ms cap, independent streak, typed counters, exact parent retained\n";
}

void exercise_suffix_clock() {
  RecoveryFixture fixture(2);
  fixture.parents[1]->first_retry_at = td::Time::now() - 185;
  fixture.fail_parent(1, "native transfer run admission snapshot changed; retry");
  CHECK(fixture.parents[1]->source_head_retry_at < 0 && !fixture.worker->wallets_[0].retry_quarantined);
  fixture.worker->accept_task(fixture.parents[0], NativeLoadWorker::TaskResolution::admitted);
  CHECK(fixture.parents[1]->source_head_retry_at >= td::Time::now() - 1);
  fixture.fail_parent(1, "native transfer run admission snapshot changed; retry");
  CHECK(!fixture.worker->wallets_[0].retry_quarantined && fixture.worker->stats_.retry_exhausted == 0);
  std::cout << "suffix_head_clock: PASS 185s behind predecessor does not exhaust newly promoted head\n";
}


void exercise_scalar_repair_and_conflict() {
  RecoveryFixture fixture(2);
  auto& worker = *fixture.worker;
  auto& wallet = worker.wallets_[0];
  worker.options_.native_signed_runs.requested = false;
  worker.active_tasks_ = worker.canonical_backlog_ = 2;
  worker.stats_.offered = worker.stats_.steady_offered = 2;
  wallet.tasks.clear();
  wallet.expected_hashes.clear();
  wallet.next_nonce = wallet.steady_end_nonce = 102;
  for (std::size_t i = 0; i < 2; ++i) {
    auto& task = fixture.parents[i];
    task->signed_run = false;
    task->transfer.nonce = 100 + i;
    task->transfer.valid_until = fixture.valid_until;
    task->proof_observed.assign(1, false);
    wallet.tasks.emplace(task->first_nonce(), task);
    worker.remember_expected_hash(task, fixture.hashes[i]);
  }
  auto lower = fixture.parents[0];
  auto later = fixture.parents[1];
  later->first_retry_at = td::Time::now() - 185;
  fixture.fail_parent(1, "validator not ready");
  CHECK(later->source_head_retry_at < 0);
  worker.accept_task(lower, NativeLoadWorker::TaskResolution::admitted);
  CHECK(later->source_head_retry_at >= td::Time::now()-1);
  worker.sending_done_ = true;
  worker.drain_deadline_ = td::Time::now()+180;
  later->source_head_retry_at = td::Time::now()-185;
  worker.repair_gaps();
  CHECK(wallet.tasks.at(100) == lower && lower->admission_counted);
  CHECK(lower->boc.as_slice().str() == fixture.bytes[0] && later->source_head_retry_at < 0);
  worker.accept_task(lower, NativeLoadWorker::TaskResolution::admitted);
  CHECK(worker.stats_.mempool_accepted == 1 && worker.stats_.repeat_admission_successes == 1);
  CHECK(later->source_head_retry_at >= td::Time::now()-1);
  later->source_head_retry_at = td::Time::now()-185;
  fixture.fail_parent(1, "validator not ready");
  CHECK(wallet.retry_quarantined && !wallet.disabled && worker.stats_.retry_exhausted == 1);
  wallet.last_repair_at = td::Time::now()-11;
  worker.repair_gaps();
  CHECK(wallet.tasks.at(100) == lower && lower->boc.as_slice().str() == fixture.bytes[0]);
  worker.accept_task(lower, NativeLoadWorker::TaskResolution::admitted);
  worker.schedule_retry(later, 10.0, TaskErrorReason::not_ready);
  CHECK(worker.stats_.retry_exhausted == 1 && worker.stats_.repeat_admission_successes == 2);
  CHECK(later->retry_at <= td::Time::now()+2.001);
  worker.disable_wallet_for_conflict(0, "fixture conflicting canonical parent");
  CHECK(wallet.disabled && wallet.tasks.empty() && worker.active_tasks_ == 0);
  worker.repair_gaps();
  CHECK(wallet.tasks.empty() && !worker.find_available_wallet());
  std::cout << "scalar_repair_and_conflict: PASS suffix clock, earlier exact repair, idempotent repeated admission, one exhaustion count, hard conflict quarantine\n";
}

void exercise_due_retry_pump() {
  RecoveryFixture fixture(1);
  auto& worker = *fixture.worker;
  auto task = fixture.parents[0];
  worker.started_ = worker.steady_started_ = worker.sending_done_ = true;
  worker.drain_deadline_ = td::Time::now()+180;
  const auto deadline = worker.drain_deadline_;
  worker.clients_[0].cwnd = 1;  // Deliberately prevent an indivisible 16-child dispatch.
  task->first_retry_at = task->source_head_retry_at = td::Time::now()-185;
  task->retry_at = td::Time::now()-1;
  task->retry_reason = TaskErrorReason::not_ready;
  task->last_error_code = ton::ErrorCode::notready;
  task->last_error_message = "fixture previously observed not-ready response";
  worker.retry_tasks_.emplace(task->retry_at, task);
  worker.pump();
  CHECK(worker.wallets_[0].retry_quarantined && !worker.wallets_[0].disabled);
  CHECK(worker.stats_.retry_exhausted == 1 && worker.active_tasks_ == 16);
  CHECK(task->state == NativeLoadWorker::TaskState::ready && !worker.ready_wallets_.empty());
  CHECK(worker.retry_tasks_.empty() && worker.inflight_ == 0 && worker.drain_deadline_ == deadline);
  fixture.check_identity();
  std::cout << "due_retry_pump: PASS expired head retry remains ready with exact parent and unchanged drain deadline\n";
}

void exercise_reconciliation(td::actor::ActorId<NativeLoadCoordinator> coordinator) {
  RecoveryFixture fixture(8, coordinator);
  fixture.trigger_head_horizon();
  auto& worker = *fixture.worker;
  const auto drain_deadline = td::Time::now() + 180;
  worker.sending_done_ = true;
  worker.drain_started_at_ = td::Time::now();
  worker.drain_deadline_ = drain_deadline;
  for (std::size_t i = 0; i < fixture.parents.size(); ++i) {
    worker.mark_task_ready(fixture.parents[i]);
    auto next = worker.take_dispatchable_ready_task(true);
    CHECK(next == fixture.parents[i]);
    // Deliver the real successful admission callback for that exact parent.
    next->state = NativeLoadWorker::TaskState::inflight;
    next->last_sent_at = td::Time::now();
    worker.inflight_ = 16;
    worker.clients_[0].inflight = 16;
    worker.clients_[0].admission_queries_inflight = 1;
    auto response = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_sendMsgStatus>(1), true);
    worker.on_result(next, 0, std::move(response));
    CHECK(worker.drain_deadline_ == drain_deadline && worker.stats_.retry_exhausted == 1);
  }
  CHECK(worker.active_tasks_ == 0 && worker.wallets_[0].tasks.empty());
  CHECK(worker.wallets_[0].admitted_tasks.size() == 8 && worker.stats_.mempool_accepted == 128);
  CHECK(worker.stats_.canonical_hash_matched == 0 && !worker.finished_);
  fixture.check_identity();
  worker.started_ = true;
  worker.steady_started_ = true;
  std::vector<CanonicalTransferObservation> observations;
  for (std::size_t i = 0; i < fixture.parents.size(); ++i) {
    for (td::uint64 nonce = fixture.parents[i]->first_nonce(); nonce < fixture.parents[i]->first_nonce()+16; ++nonce) {
      observations.push_back({0, nonce, fixture.hashes[i]});
    }
  }
  // The unchanged worker consumes follower observations with exact source,
  // nonce and parent hashes; merely successful admission could not finish it.
  worker.observe_canonical_transfers(std::move(observations));
  worker.canonical_checkpoint(true);
  CHECK(worker.stats_.canonical_hash_matched == 128 && worker.stats_.native_signed_run_proof_resolutions == 8);
  CHECK(worker.stats_.total_anchored_after_drain == 128 && worker.stats_.anchored_after_drain == 128);
  CHECK(worker.wallets_[0].admitted_tasks.empty() && worker.wallets_[0].expected_hashes.empty());
  CHECK(worker.finished_ && !worker.stats_.drain_timed_out && worker.canonical_backlog_ == 0);
  CHECK(worker.stats_.retry_exhausted == 1 && worker.wallets_[0].retry_quarantined);
  CHECK(worker.drain_deadline_ == drain_deadline);
  std::cout << "quarantine_128_logical_recovery: PASS retains8parents, stops new offers, retries in order, requires all128 exact canonical children, keeps capacity invalid\n";
}

void exercise_deadline(td::actor::ActorId<NativeLoadCoordinator> coordinator) {
  RecoveryFixture fixture(1, coordinator);
  fixture.trigger_head_horizon();
  auto& worker = *fixture.worker;
  worker.sending_done_ = true;
  worker.drain_deadline_ = td::Time::now() - 1;
  const auto deadline = worker.drain_deadline_;
  worker.maybe_finish();
  CHECK(worker.finished_ && worker.stats_.drain_timed_out);
  CHECK(worker.drain_deadline_ == deadline && worker.stats_.canonical_hash_matched == 0);
  fixture.check_identity();
  std::cout << "unresolved_deadline: PASS retains invalid timed-out verdict; no deadline extension or invented proof\n";
}
}
int main() {
  exercise_deadline_callback(false);
  exercise_deadline_callback(true);
  exercise_bounded_revision_retry();
  exercise_suffix_clock();
  exercise_scalar_repair_and_conflict();
  exercise_due_retry_pump();
  td::actor::Scheduler scheduler({0}, false, td::actor::Scheduler::Paused);
  scheduler.run_in_context([&] {
    Options sink_options;
    sink_options.global_config = "/dev/null";
    auto coordinator = td::actor::create_actor<NativeLoadCoordinator>("fixture-unstarted-coordinator", sink_options);
    exercise_reconciliation(coordinator.get());
    exercise_deadline(coordinator.get());
  });
  scheduler.stop();
  return 0;
}
