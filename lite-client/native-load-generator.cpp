/*
    High-rate native-transfer load generator for a private TON sidechain.

    It intentionally runs as a standalone process/container.  Keys are loaded
    once, Ed25519 private keys are prepared once, BOCs are built in memory, and
    multiple persistent ADNL/TCP lite-server connections keep a bounded number
    of sendMessage queries in flight.
*/
#include "auto/tl/lite_api.hpp"
#include "auto/tl/ton_api_json.h"
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
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Options {
  std::string global_config{"/usr/share/data/global.config.json"};
  std::string wallet_dir{"/wallets"};
  td::uint32 sources{1000};
  td::uint32 connections{4};
  td::uint32 signers{4};
  td::uint32 max_inflight{8192};
  td::uint32 duration_seconds{60};
  td::uint32 valid_for_seconds{120};
  td::uint64 amount{1};
  td::uint64 fee{0};
  td::uint64 start_nonce{0};
  double query_timeout{10.0};
  double report_interval{1.0};
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

class NativeLoadGenerator final : public td::actor::Actor {
 public:
  explicit NativeLoadGenerator(Options options) : options_(std::move(options)) {
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

  struct Wallet {
    ton::StdSmcAddress source;
    ton::StdSmcAddress destination;
    std::shared_ptr<const td::Ed25519::PreparedPrivateKey> private_key;
    td::uint64 next_nonce{0};
  };

  Options options_;
  std::vector<Wallet> wallets_;
  std::vector<td::actor::ActorOwn<liteclient::ExtClient>> clients_;
  std::vector<td::actor::ActorOwn<Signer>> signers_;
  std::size_t wallet_cursor_{0};
  std::size_t client_cursor_{0};
  std::size_t signer_cursor_{0};
  td::uint64 submitted_{0};
  td::uint64 admitted_{0};
  td::uint64 rejected_{0};
  td::uint64 errors_{0};
  td::uint64 inflight_{0};
  td::uint64 signing_{0};
  td::uint64 last_submitted_{0};
  td::uint64 last_admitted_{0};
  double started_at_{0};
  double last_report_at_{0};
  bool sending_done_{false};

  void start_up() override {
    auto status = initialize();
    if (status.is_error()) {
      LOG(FATAL) << status;
    }
    started_at_ = last_report_at_ = td::Time::now();
    LOG(WARNING) << "native load generator started: sources=" << wallets_.size()
                 << " connections=" << clients_.size() << " signers=" << signers_.size()
                 << " max_inflight=" << options_.max_inflight
                 << " duration=" << options_.duration_seconds << "s amount=" << options_.amount
                 << "ng fee=" << options_.fee << "ng start_nonce=" << options_.start_nonce;
    pump();
    alarm_timestamp() = td::Timestamp::in(std::min(0.1, options_.report_interval));
  }

  td::Status initialize() {
    TRY_RESULT(config_data, td::read_file(options_.global_config));
    TRY_RESULT(config_json, td::json_decode(config_data.as_slice()));
    ton::ton_api::liteclient_config_global global;
    TRY_STATUS(ton::ton_api::from_json(global, config_json.get_object()));
    TRY_RESULT(servers, liteclient::LiteServerConfig::parse_global_config(global));
    if (servers.empty()) {
      return td::Status::Error("global config has no liteservers");
    }
    clients_.reserve(options_.connections);
    for (td::uint32 i = 0; i < options_.connections; ++i) {
      clients_.push_back(liteclient::ExtClient::create(servers, nullptr));
    }
    signers_.reserve(options_.signers);
    for (td::uint32 i = 0; i < options_.signers; ++i) {
      signers_.push_back(td::actor::create_actor<Signer>("native-load-signer"));
    }

    wallets_.reserve(options_.sources);
    for (td::uint32 i = 0; i < options_.sources; ++i) {
      auto prefix = options_.wallet_dir + "/source-" + std::to_string(i);
      auto destination = options_.wallet_dir + "/dest-" + std::to_string(i) + ".pub";
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
      wallets_.push_back(Wallet{source, destination_address, std::move(prepared), options_.start_nonce});
    }
    return td::Status::OK();
  }

  td::Result<block::NativeTransfer> reserve_transfer(Wallet& wallet) {
    block::NativeTransfer transfer;
    transfer.src = wallet.source;
    transfer.dst = wallet.destination;
    transfer.amount = options_.amount;
    transfer.fee = options_.fee;
    transfer.nonce = wallet.next_nonce;
    auto expires = td::Clocks::system() + options_.valid_for_seconds;
    if (expires >= std::numeric_limits<ton::UnixTime>::max()) {
      return td::Status::Error("valid_until overflows uint32");
    }
    transfer.valid_until = static_cast<ton::UnixTime>(expires);
    ++wallet.next_nonce;
    return transfer;
  }

  void pump() {
    if (sending_done_) {
      return;
    }
    if (td::Time::now() - started_at_ >= options_.duration_seconds) {
      sending_done_ = true;
      maybe_finish();
      return;
    }
    while (signing_ + inflight_ < options_.max_inflight && !sending_done_) {
      Wallet& wallet = wallets_[wallet_cursor_++ % wallets_.size()];
      auto transfer = reserve_transfer(wallet);
      if (transfer.is_error()) {
        LOG(ERROR) << transfer.error();
        ++errors_;
        sending_done_ = true;
        maybe_finish();
        return;
      }
      auto promise = td::PromiseCreator::lambda(
          [self = actor_id(this)](td::Result<td::BufferSlice> message) mutable {
            td::actor::send_closure(self, &NativeLoadGenerator::on_signed, std::move(message));
          });
      auto& signer = signers_[signer_cursor_++ % signers_.size()];
      td::actor::send_closure(signer, &Signer::sign, transfer.move_as_ok(), wallet.private_key, std::move(promise));
      ++signing_;
      if (td::Time::now() - started_at_ >= options_.duration_seconds) {
        sending_done_ = true;
      }
    }
  }

  void on_signed(td::Result<td::BufferSlice> message) {
    CHECK(signing_ > 0);
    --signing_;
    if (message.is_error()) {
      LOG(ERROR) << message.error();
      ++errors_;
    } else {
      auto query = ton::serialize_tl_object(
          ton::create_tl_object<ton::lite_api::liteServer_sendMessage>(message.move_as_ok()), true);
      auto envelope = ton::serialize_tl_object(
          ton::create_tl_object<ton::lite_api::liteServer_query>(std::move(query)), true);
      auto promise = td::PromiseCreator::lambda(
          [self = actor_id(this)](td::Result<td::BufferSlice> result) mutable {
            td::actor::send_closure(self, &NativeLoadGenerator::on_result, std::move(result));
          });
      auto& client = clients_[client_cursor_++ % clients_.size()];
      td::actor::send_closure(client, &liteclient::ExtClient::send_query, "native-load", std::move(envelope),
                              td::Timestamp::in(options_.query_timeout), std::move(promise));
      ++submitted_;
      ++inflight_;
    }
    pump();
    maybe_finish();
  }

  void on_result(td::Result<td::BufferSlice> result) {
    CHECK(inflight_ > 0);
    --inflight_;
    if (result.is_error()) {
      ++errors_;
    } else {
      auto parsed = ton::fetch_tl_object<ton::lite_api::liteServer_sendMsgStatus>(result.move_as_ok(), true);
      if (parsed.is_error()) {
        ++errors_;
      } else if (parsed.move_as_ok()->status_ == 1) {
        ++admitted_;
      } else {
        ++rejected_;
      }
    }
    pump();
    maybe_finish();
  }

  void alarm() override {
    auto now = td::Time::now();
    if (now - last_report_at_ >= options_.report_interval) {
      auto interval = now - last_report_at_;
      auto submitted_rate = static_cast<double>(submitted_ - last_submitted_) / interval;
      auto admitted_rate = static_cast<double>(admitted_ - last_admitted_) / interval;
      std::cout << "{\"elapsed_s\":" << (now - started_at_) << ",\"submitted\":" << submitted_
                << ",\"admitted\":" << admitted_ << ",\"rejected\":" << rejected_ << ",\"errors\":"
                << errors_ << ",\"inflight\":" << inflight_ << ",\"submit_tps\":" << submitted_rate
                << ",\"signing\":" << signing_
                << ",\"admit_tps\":" << admitted_rate << "}" << std::endl;
      last_report_at_ = now;
      last_submitted_ = submitted_;
      last_admitted_ = admitted_;
    }
    pump();
    maybe_finish();
    if (!sending_done_ || inflight_ || signing_) {
      alarm_timestamp() = td::Timestamp::in(std::min(0.1, options_.report_interval));
    }
  }

  void maybe_finish() {
    if (!sending_done_ || inflight_ || signing_) {
      return;
    }
    auto elapsed = std::max(0.000001, td::Time::now() - started_at_);
    LOG(WARNING) << "native load generator finished: elapsed=" << elapsed << "s submitted=" << submitted_
                 << " admitted=" << admitted_ << " rejected=" << rejected_ << " errors=" << errors_
                 << " submit_tps=" << static_cast<double>(submitted_) / elapsed
                 << " admit_tps=" << static_cast<double>(admitted_) / elapsed;
    stop();
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_WARNING);
  td::set_default_failure_signal_handler();
  Options options;
  td::OptionParser parser;
  parser.set_description("Persistent high-rate native-transfer load generator");
  parser.add_option('h', "help", "show this help", [&] {
    char buffer[16384];
    td::StringBuilder builder(td::MutableSlice{buffer, sizeof(buffer)});
    builder << parser;
    std::cout << builder.as_cslice().c_str();
    std::exit(0);
  });
  parser.add_option('C', "global-config", "path to global.config.json",
                    [&](td::Slice value) { options.global_config = value.str(); });
  parser.add_option('W', "wallet-dir", "directory containing source-N.{pk,pub} and dest-N.pub",
                    [&](td::Slice value) { options.wallet_dir = value.str(); });
  parser.add_checked_option('s', "sources", "number of source/destination pairs", [&](td::Slice value) {
    options.sources = td::to_integer<td::uint32>(value);
    return options.sources ? td::Status::OK() : td::Status::Error("sources must be positive");
  });
  parser.add_checked_option('c', "connections", "persistent ADNL/TCP connections", [&](td::Slice value) {
    options.connections = td::to_integer<td::uint32>(value);
    return options.connections && options.connections <= 64 ? td::Status::OK()
                                                            : td::Status::Error("connections must be 1..64");
  });
  parser.add_checked_option('S', "signers", "parallel in-memory signing workers", [&](td::Slice value) {
    options.signers = td::to_integer<td::uint32>(value);
    return options.signers && options.signers <= 64 ? td::Status::OK()
                                                    : td::Status::Error("signers must be 1..64");
  });
  parser.add_checked_option('i', "inflight", "maximum outstanding sendMessage queries", [&](td::Slice value) {
    options.max_inflight = td::to_integer<td::uint32>(value);
    return options.max_inflight ? td::Status::OK() : td::Status::Error("inflight must be positive");
  });
  parser.add_checked_option('d', "duration", "load duration in seconds", [&](td::Slice value) {
    options.duration_seconds = td::to_integer<td::uint32>(value);
    return options.duration_seconds ? td::Status::OK() : td::Status::Error("duration must be positive");
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
  parser.add_option('n', "start-nonce", "initial nonce for every source",
                    [&](td::Slice value) { options.start_nonce = td::to_integer<td::uint64>(value); });
  parser.add_option('t', "query-timeout", "liteserver query timeout in seconds",
                    [&](td::Slice value) { options.query_timeout = td::to_double(value); });
  parser.add_option('r', "report-interval", "JSON metrics interval in seconds",
                    [&](td::Slice value) { options.report_interval = td::to_double(value); });
  parser.run(argc, argv).ensure();
  CHECK(options.query_timeout > 0 && options.report_interval > 0);

  vm::init_vm(true).ensure();
  auto scheduler_threads = std::clamp(options.signers + 1, 2u, 65u);
  td::actor::Scheduler scheduler({scheduler_threads});
  td::actor::ActorOwn<NativeLoadGenerator> generator;
  scheduler.run_in_context([&] {
    generator = td::actor::create_actor<NativeLoadGenerator>("native-load-generator", std::move(options));
    generator.release();
  });
  scheduler.run();
  return 0;
}
