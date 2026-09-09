#include <cstdlib>
#include <optional>
#include <string>

#include "common/util.h"
#include "keyring/keyring.h"
#include "keys/encryptor.h"
#include "overlay/overlay.hpp"
#include "td/actor/TestScheduler.h"
#include "td/actor/coro_utils.h"
#include "td/utils/tests.h"

namespace ton::overlay {

struct LocalBroadcastSignatureTestAccess {
  static LocalBroadcastSignature request(PublicKeyHash signer, td::Slice bytes) {
    return LocalBroadcastSignature(signer, bytes);
  }
  struct Stats {
    bool enabled;
    td::uint64 crypto_checks, receipt_checks, hits, mismatches;
  };
  static Stats stats(const OverlayImpl &overlay) {
    return {overlay.local_signature_reuse_enabled_, overlay.signature_crypto_checks_,
            overlay.local_signature_receipt_checks_, overlay.local_signature_reuse_hits_,
            overlay.local_signature_receipt_mismatches_};
  }
  static void ban(OverlayImpl &overlay, adnl::AdnlNodeIdShort peer) {
    overlay.reject_signatures_from_.insert(peer);
  }
};

namespace {

PrivateKey fixed_key(unsigned char seed = 1) {
  auto bytes = td::Bits256::zero();
  bytes.as_array()[0] = seed;
  return PrivateKey{privkeys::Ed25519{bytes}};
}

using SignResult = td::Result<std::pair<td::BufferSlice, PublicKey>>;
SignResult signed_result(const PrivateKey &key, td::Slice bytes) {
  return std::pair{key.create_decryptor().move_as_ok()->sign(bytes).move_as_ok(), key.compute_public_key()};
}

class ScopedFlag {
 public:
  explicit ScopedFlag(const char *value) {
    if (const char *old = std::getenv("TON_OVERLAY_LOCAL_SIGNATURE_REUSE")) previous_ = old;
    set(value);
  }
  ~ScopedFlag() { set(previous_ ? previous_->c_str() : nullptr); }
  static void set(const char *value) {
    if (value) {
      CHECK(setenv("TON_OVERLAY_LOCAL_SIGNATURE_REUSE", value, 1) == 0);
    } else {
      CHECK(unsetenv("TON_OVERLAY_LOCAL_SIGNATURE_REUSE") == 0);
    }
  }
 private:
  std::optional<std::string> previous_;
};

class CaptureCallback final : public Overlays::Callback {
 public:
  explicit CaptureCallback(std::shared_ptr<std::vector<std::string>> received) : received_(std::move(received)) {}
  void receive_broadcast(PublicKeyHash, OverlayIdShort, td::BufferSlice data) override {
    received_->push_back(data.as_slice().str());
  }
 private:
  std::shared_ptr<std::vector<std::string>> received_;
};

class TestOverlay final : public OverlayImpl {
 public:
  TestOverlay(td::actor::ActorId<keyring::Keyring> keyring, PublicKeyHash source,
              std::shared_ptr<std::vector<std::string>> received, bool allowed = true)
      : OverlayImpl(keyring, {}, {}, {}, adnl::AdnlNodeIdShort::zero(),
                    OverlayIdFull{td::BufferSlice("local-signature-test")}, OverlayType::FixedMemberList, {}, {}, {},
                    std::make_unique<CaptureCallback>(std::move(received)),
                    OverlayPrivacyRules{0, 0, {{source, allowed ? 1048576u : 0u}}}) {}
  void start_up() override {}
  void alarm() override {}
  auto stats() { return LocalBroadcastSignatureTestAccess::stats(*this); }
  void send_test_fec(PublicKeyHash source, td::BufferSlice data) {
    auto hash = td::sha256_bits256(data.as_slice());
    fec::FecType type{td::fec::RaptorQEncoder::Parameters{data.size(), data.size(), 0}};
    auto encoder = type.create_encoder(std::move(data)).move_as_ok();
    auto symbol = encoder->gen_symbol(0);
    send_new_fec_broadcast_part(source, hash, type.size(), 0, std::move(symbol.data), 0, type,
                                static_cast<td::uint32>(td::Clocks::system()));
  }
};

auto simple_wire(const PrivateKey &key, td::Slice data, td::uint32 flags = 0) {
  auto source = key.compute_public_key();
  auto broadcast_hash = get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcast_id>(
      flags & Overlays::BroadcastFlagAnySender() ? PublicKeyHash::zero().tl() : source.compute_short_id().tl(),
      td::sha256_bits256(data), flags));
  auto date = static_cast<td::uint32>(td::Clocks::system());
  auto message = create_serialize_tl_object<ton_api::overlay_broadcast_toSign>(broadcast_hash, date);
  auto signature = key.create_decryptor().move_as_ok()->sign(message.as_slice()).move_as_ok();
  return create_tl_object<ton_api::overlay_broadcast>(source.tl(), Certificate::empty_tl(), flags,
                                                     td::BufferSlice(data), date, std::move(signature));
}

struct FecWire {
  PrivateKey key = fixed_key();
  td::BufferSlice data{"fec-wire-data"};
  fec::FecType type{td::fec::RaptorQEncoder::Parameters{data.size(), data.size(), 0}};
  std::unique_ptr<td::fec::Encoder> encoder;
  td::uint32 date = static_cast<td::uint32>(td::Clocks::system());
  td::uint32 flags;
  td::Bits256 data_hash = td::sha256_bits256(data.as_slice());
  td::Bits256 broadcast_hash;
  explicit FecWire(td::uint32 f = 0) : flags(f) {
    encoder = type.create_encoder(data.copy()).move_as_ok();
    encoder->prepare_more_symbols();
    broadcast_hash = get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastFec_id>(
        flags & Overlays::BroadcastFlagAnySender() ? PublicKeyHash::zero().tl() : key.compute_short_id().tl(),
        get_tl_object_sha_bits256(type.tl()), data_hash, type.size(), flags));
  }
  td::BufferSlice signature(td::uint32 seqno) {
    auto part = encoder->gen_symbol(seqno);
    auto part_hash = get_tl_object_sha_bits256(
        create_tl_object<ton_api::overlay_broadcastFec_partId>(broadcast_hash, td::sha256_bits256(part.data.as_slice()), seqno));
    auto bytes = create_serialize_tl_object<ton_api::overlay_broadcast_toSign>(part_hash, date);
    return key.create_decryptor().move_as_ok()->sign(bytes.as_slice()).move_as_ok();
  }
  auto full(td::uint32 seqno = 1) {
    auto part = encoder->gen_symbol(seqno);
    return create_tl_object<ton_api::overlay_broadcastFec>(key.compute_public_key().tl(), Certificate::empty_tl(),
        data_hash, type.size(), flags, std::move(part.data), seqno, type.tl(), date, signature(seqno));
  }
  auto short_part() {
    return create_tl_object<ton_api::overlay_broadcastFecShort>(key.compute_public_key().tl(), Certificate::empty_tl(),
        broadcast_hash, data_hash, 0, signature(0));
  }
};

}  // namespace

TEST(OverlayLocalSignature, ReceiptBindsImmutableBytesKeyAndSignature) {
  auto key = fixed_key();
  td::BufferSlice original{"domain:payload"};
  auto request = LocalBroadcastSignatureTestAccess::request(key.compute_short_id(), original.as_slice());
  auto result = signed_result(key, original.as_slice());
  auto signature = result.ok().first.copy();
  request.complete(result);
  ASSERT_TRUE(request.matches(key.compute_public_key(), original.as_slice(), signature.as_slice()));
  original.as_slice()[0] ^= 1;
  auto completed = result.move_as_ok();
  completed.first.as_slice()[0] ^= 1;
  ASSERT_TRUE(!request.matches(key.compute_public_key(), original.as_slice(), signature.as_slice()));
  ASSERT_TRUE(!request.matches(key.compute_public_key(), "domain:payload", completed.first.as_slice()));
  ASSERT_TRUE(!request.matches(fixed_key(2).compute_public_key(), "domain:payload", signature.as_slice()));
  ASSERT_TRUE(request.matches(key.compute_public_key(), "domain:payload", signature.as_slice()));
}

TEST(OverlayLocalSignature, FailedWrongUnsupportedAndMalformedCompletionsNeverMintReceipt) {
  auto key = fixed_key();
  for (int failure = 0; failure < 5; ++failure) {
    auto request = LocalBroadcastSignatureTestAccess::request(key.compute_short_id(), "message");
    SignResult result = signed_result(key, "message");
    if (failure == 0) result = td::Status::Error("keyring unavailable");
    if (failure == 1) result = signed_result(fixed_key(2), "message");
    if (failure == 2) result = std::pair{td::BufferSlice(64), PublicKey{}};
    if (failure == 3) result = std::pair{td::BufferSlice(63), key.compute_public_key()};
    if (failure == 4) result = std::pair{td::BufferSlice{}, PublicKey{pubkeys::Unenc{td::Slice("unencrypted")}}};
    request.complete(result);
    auto good = signed_result(key, "message");
    request.complete(good);  // A failed completion cannot later be upgraded.
    ASSERT_TRUE(!request.matches(key.compute_public_key(), "message", good.ok().first.as_slice()));
  }
  auto unsupported = PublicKey{pubkeys::Unenc{td::Slice("unencrypted")}};
  auto request = LocalBroadcastSignatureTestAccess::request(unsupported.compute_short_id(), "message");
  SignResult same_key = std::pair{td::BufferSlice(64), unsupported};
  request.complete(same_key);
  ASSERT_TRUE(!request.matches(unsupported, "message", same_key.ok().first.as_slice()));
}

TEST(OverlayLocalSignature, FlagIsDefaultOffStrictAndCapturedAtConstruction) {
  for (const char *value : {static_cast<const char *>(nullptr), "", "0", "true", "1"}) {
    ScopedFlag flag(value);
    TestOverlay overlay({}, fixed_key().compute_short_id(), std::make_shared<std::vector<std::string>>());
    auto expected = value && td::Slice(value) == "1";
    ScopedFlag::set(expected ? "0" : "1");
    ASSERT_EQ(overlay.local_signature_reuse_enabled(), expected);
  }
}

TEST(OverlayLocalSignature, ExactReceiptReusesWhileMismatchControlAndBanKeepVerifierSemantics) {
  auto key = fixed_key();
  auto result = signed_result(key, "message");
  auto request = LocalBroadcastSignatureTestAccess::request(key.compute_short_id(), "message");
  request.complete(result);
  for (const char *enabled : {"0", "1"}) {
    ScopedFlag flag(enabled);
    TestOverlay overlay({}, key.compute_short_id(), std::make_shared<std::vector<std::string>>());
    auto peer = adnl::AdnlNodeIdShort::zero();
    overlay.check_signature_from_peer(key.compute_public_key(), "message", result.ok().first.as_slice(), peer,
                                       &request).ensure();
    ASSERT_EQ(overlay.stats().hits, td::Slice(enabled) == "1" ? 1u : 0u);
    ASSERT_EQ(overlay.stats().crypto_checks, td::Slice(enabled) == "1" ? 0u : 1u);
    ASSERT_TRUE(overlay.check_signature_from_peer(key.compute_public_key(), "changed", result.ok().first.as_slice(),
                                                  peer, &request).is_error());
    auto changed = result.ok().first.copy();
    changed.as_slice()[0] ^= 1;
    ASSERT_TRUE(overlay.check_signature_from_peer(key.compute_public_key(), "message", changed.as_slice(), peer,
                                                  &request).is_error());
    ASSERT_TRUE(overlay.check_signature_from_peer(fixed_key(2).compute_public_key(), "message",
                                                  result.ok().first.as_slice(), peer, &request).is_error());
    auto checks = overlay.stats().crypto_checks;
    LocalBroadcastSignatureTestAccess::ban(overlay, peer);
    ASSERT_TRUE(overlay.check_signature_from_peer(key.compute_public_key(), "message", result.ok().first.as_slice(),
                                                  peer, &request).is_error());
    ASSERT_EQ(overlay.stats().crypto_checks, checks);
  }
}

TEST(OverlayLocalSignature, RealLocalKeyringCallbacksDeliverSimpleAndFecWithBothModes) {
  for (const char *enabled : {"0", "1"}) {
    ScopedFlag flag(enabled);
    auto key = fixed_key();
    auto received = std::make_shared<std::vector<std::string>>();
    td::actor::TestScheduler scheduler;
    scheduler.run([&]() -> td::actor::Task<> {
      auto ring = keyring::Keyring::create("");
      co_await td::actor::ask(ring.get(), &keyring::Keyring::add_key, key, true);
      auto overlay = td::actor::create_actor<TestOverlay>("test-overlay", ring.get(), key.compute_short_id(), received);
      co_await td::actor::ask(overlay.get(), &OverlayImpl::send_broadcast, key.compute_short_id(), 0,
                              td::BufferSlice("local-simple"));
      co_await td::actor::ask(overlay.get(), &TestOverlay::send_test_fec, key.compute_short_id(),
                              td::BufferSlice("local-fec"));
      co_await scheduler.wait_sync_work();
      auto stats = co_await td::actor::ask(overlay.get(), &TestOverlay::stats);
      ASSERT_EQ(received->size(), 2u);
      ASSERT_EQ(stats.hits, td::Slice(enabled) == "1" ? 2u : 0u);
      ASSERT_EQ(stats.crypto_checks, td::Slice(enabled) == "1" ? 0u : 2u);
      ASSERT_EQ(stats.mismatches, 0u);
      co_return {};
    });
  }
}

TEST(OverlayLocalSignature, KeyringErrorsAndForbiddenLocalFecStillReject) {
  ScopedFlag flag("1");
  auto key = fixed_key();
  auto received = std::make_shared<std::vector<std::string>>();
  td::actor::TestScheduler scheduler;
  scheduler.run([&]() -> td::actor::Task<> {
    auto ring = keyring::Keyring::create("");
    auto overlay = td::actor::create_actor<TestOverlay>("test-overlay", ring.get(), key.compute_short_id(), received);
    co_await td::actor::ask(overlay.get(), &OverlayImpl::send_broadcast, key.compute_short_id(), 0,
                            td::BufferSlice("missing-key"));
    co_await td::actor::ask(overlay.get(), &TestOverlay::send_test_fec, key.compute_short_id(),
                            td::BufferSlice("missing-fec-key"));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(received->size(), 0u);
    co_await td::actor::ask(ring.get(), &keyring::Keyring::add_key, key, true);
    auto forbidden = td::actor::create_actor<TestOverlay>("forbidden-overlay", ring.get(), key.compute_short_id(),
                                                          received, false);
    co_await td::actor::ask(forbidden.get(), &TestOverlay::send_test_fec, key.compute_short_id(),
                            td::BufferSlice("forbidden-fec"));
    co_await scheduler.wait_sync_work();
    ASSERT_EQ(received->size(), 0u);
    auto stats = co_await td::actor::ask(forbidden.get(), &TestOverlay::stats);
    ASSERT_EQ(stats.hits, 0u);
    ASSERT_EQ(stats.crypto_checks, 0u);  // Eligibility is still checked first.
    co_return {};
  });
}

TEST(OverlayLocalSignature, IncomingSimpleWithZeroPeerAndClaimedLocalKeyAlwaysVerifies) {
  ScopedFlag flag("1");
  auto key = fixed_key();
  for (auto flags : {0u, Overlays::BroadcastFlagAnySender()}) {
    auto received = std::make_shared<std::vector<std::string>>();
    TestOverlay overlay({}, key.compute_short_id(), received);
    BroadcastsSimple broadcasts;
    auto bad = simple_wire(key, "wire-simple", flags);
    bad->signature_.as_slice()[0] ^= 1;
    ASSERT_TRUE(broadcasts.process_broadcast(&overlay, adnl::AdnlNodeIdShort::zero(), std::move(bad)).is_error());
    ASSERT_EQ(received->size(), 0u);
    broadcasts.process_broadcast(&overlay, adnl::AdnlNodeIdShort::zero(), simple_wire(key, "wire-simple", flags)).ensure();
    ASSERT_EQ(received->size(), 1u);
    ASSERT_EQ(overlay.stats().crypto_checks, 2u);
    ASSERT_EQ(overlay.stats().hits, 0u);
  }
}

TEST(OverlayLocalSignature, IncomingFullAndShortFecWithZeroPeerAlwaysVerify) {
  ScopedFlag flag("1");
  for (auto flags : {0u, Overlays::BroadcastFlagAnySender()}) {
    FecWire wire(flags);
    auto received = std::make_shared<std::vector<std::string>>();
    TestOverlay overlay({}, wire.key.compute_short_id(), received);
    BroadcastsFec broadcasts;
    auto bad = wire.full();
    bad->signature_.as_slice()[0] ^= 1;
    ASSERT_TRUE(broadcasts.process_broadcast(&overlay, adnl::AdnlNodeIdShort::zero(), std::move(bad)).is_error());
    // Decode through repair symbols. Systematic symbol zero remains unseen,
    // and the reconstructed encoder is ready to serve that short part.
    for (td::uint32 seqno = 1; seqno < 6 && received->empty(); ++seqno) {
      broadcasts.process_broadcast(&overlay, adnl::AdnlNodeIdShort::zero(), wire.full(seqno)).ensure();
    }
    ASSERT_EQ(received->size(), 1u);
    auto previous_checks = overlay.stats().crypto_checks;
    auto bad_short = wire.short_part();
    bad_short->signature_.as_slice()[0] ^= 1;
    ASSERT_TRUE(broadcasts.process_broadcast(&overlay, adnl::AdnlNodeIdShort::zero(), std::move(bad_short)).is_error());
    broadcasts.process_broadcast(&overlay, adnl::AdnlNodeIdShort::zero(), wire.short_part()).ensure();
    ASSERT_EQ(received->size(), 1u);
    ASSERT_EQ(overlay.stats().crypto_checks, previous_checks + 2);
    ASSERT_EQ(overlay.stats().hits, 0u);
  }
}

}  // namespace ton::overlay
