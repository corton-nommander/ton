#include <cstdlib>
#include <optional>
#include <string>

#include "keyring/keyring.hpp"
#include "keyring/prepared-key-signer.hpp"
#include "keys/encryptor.hpp"
#include "td/actor/TestScheduler.h"
#include "td/actor/coro_utils.h"
#include "td/utils/port/path.h"
#include "td/utils/tests.h"

namespace ton::keyring {

class PreparedKeySignerTestAccess {
 public:
  static auto prepared(const PreparedKeySigner &signer) {
    return std::weak_ptr<const td::Ed25519::PreparedPrivateKey>(signer.prepared_);
  }
  static bool enabled(const KeyringImpl &keyring) {
    return keyring.prepared_signing_;
  }
};

namespace {

PrivateKey fixed_key(unsigned char seed = 1) {
  auto bytes = td::Bits256::zero();
  bytes.as_array()[0] = seed;
  return PrivateKey{privkeys::Ed25519{bytes}};
}

std::unique_ptr<PreparedKeySigner> prepared_signer(const PrivateKey &key) {
  return std::make_unique<PreparedKeySigner>(key.export_as_ed25519().move_as_ok(),
                                            key.create_decryptor().move_as_ok());
}

class ScopedFlag {
 public:
  explicit ScopedFlag(const char *value) {
    if (const char *old = std::getenv("TON_KEYRING_PREPARED_SIGNING")) {
      previous_ = old;
    }
    set(value);
  }
  ~ScopedFlag() {
    set(previous_ ? previous_->c_str() : nullptr);
  }
  static void set(const char *value) {
    if (value) {
      CHECK(setenv("TON_KEYRING_PREPARED_SIGNING", value, 1) == 0);
    } else {
      CHECK(unsetenv("TON_KEYRING_PREPARED_SIGNING") == 0);
    }
  }

 private:
  std::optional<std::string> previous_;
};

class InspectableKeyring final : public KeyringImpl {
 public:
  using KeyringImpl::KeyringImpl;
  bool prepared_enabled() {
    return PreparedKeySignerTestAccess::enabled(*this);
  }
};

}  // namespace

TEST(KeyringPreparedSigning, RepeatedEmptyBinaryAndDomainMessagesAreByteIdentical) {
  auto key = fixed_key();
  auto signer = prepared_signer(key);
  auto baseline = key.create_decryptor().move_as_ok();
  auto verifier = key.compute_public_key().create_encryptor().move_as_ok();
  ASSERT_TRUE(PreparedKeySignerTestAccess::prepared(*signer).expired());
  std::vector<std::string> messages{"", std::string("\0\xff\0\x01", 4), "domain-A:payload", "domain-B:payload"};
  for (const auto &message : messages) {
    auto signature = signer->sign(message).move_as_ok();
    ASSERT_EQ(signature.as_slice(), baseline->sign(message).move_as_ok().as_slice());
    verifier->check_signature(message, signature.as_slice()).ensure();
    ASSERT_TRUE(verifier->check_signature(message + "changed", signature.as_slice()).is_error());
  }
  auto prepared = PreparedKeySignerTestAccess::prepared(*signer).lock();
  ASSERT_TRUE(prepared != nullptr);
  auto signature_a = signer->sign(messages[2]).move_as_ok();
  auto signature_b = signer->sign(messages[3]).move_as_ok();
  ASSERT_TRUE(signature_a.as_slice() != signature_b.as_slice());
  ASSERT_TRUE(PreparedKeySignerTestAccess::prepared(*signer).lock().get() == prepared.get());
}

TEST(KeyringPreparedSigning, BatchMoveAndDestructionRetainOneActorOwnedPreparation) {
  auto key = fixed_key();
  auto signer = prepared_signer(key);
  signer->sign("warm").ensure();
  auto prepared = PreparedKeySignerTestAccess::prepared(*signer);
  auto address = prepared.lock().get();
  auto moved = std::move(signer);
  auto baseline = key.create_decryptor().move_as_ok();
  std::vector<td::Slice> messages{"batch-first", "", "batch-third", "batch-first"};
  auto signatures = moved->sign_batch(messages);
  auto expected = baseline->sign_batch(messages);
  ASSERT_EQ(signatures.size(), messages.size());
  for (std::size_t i = 0; i < messages.size(); ++i) {
    ASSERT_EQ(signatures[i].move_as_ok().as_slice(), expected[i].move_as_ok().as_slice());
  }
  ASSERT_TRUE(PreparedKeySignerTestAccess::prepared(*moved).lock().get() == address);
  auto second = prepared_signer(fixed_key(2));
  auto other_signature = second->sign(messages[0]).move_as_ok();
  ASSERT_TRUE(PreparedKeySignerTestAccess::prepared(*second).lock().get() != address);
  auto verifier = key.compute_public_key().create_encryptor().move_as_ok();
  ASSERT_TRUE(verifier->check_signature(messages[0], other_signature.as_slice()).is_error());
  moved.reset();
  ASSERT_TRUE(prepared.expired());
}

TEST(KeyringPreparedSigning, ImportFailuresKeepLegacyErrorAndAreNotCached) {
  for (std::size_t size : {0u, 31u, 33u}) {
    td::Ed25519::PrivateKey key{td::SecureString(size, 'x')};
    auto expected = key.sign("payload").move_as_error().move_as_error_prefix("failed to sign: ");
    PreparedKeySigner signer{std::move(key), std::make_unique<DecryptorNone>()};
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
      auto actual = signer.sign("payload").move_as_error();
      ASSERT_EQ(actual.code(), expected.code());
      ASSERT_EQ(actual.message(), expected.message());
      ASSERT_TRUE(PreparedKeySignerTestAccess::prepared(signer).expired());
    }
  }
}

TEST(KeyringPreparedSigning, DecryptionStillUsesOriginalDecryptor) {
  auto key = fixed_key();
  auto signer = prepared_signer(key);
  auto original = key.create_decryptor().move_as_ok();
  auto encryptor = key.compute_public_key().create_encryptor().move_as_ok();
  auto encrypted = encryptor->encrypt("private payload").move_as_ok();
  ASSERT_EQ(signer->decrypt(encrypted.as_slice()).move_as_ok().as_slice(), "private payload");
  auto expected = original->decrypt("malformed").move_as_error();
  auto actual = signer->decrypt("malformed").move_as_error();
  ASSERT_EQ(actual.code(), expected.code());
  ASSERT_EQ(actual.message(), expected.message());
  ASSERT_TRUE(PreparedKeySignerTestAccess::prepared(*signer).expired());
}

TEST(KeyringPreparedSigning, FlagIsStrictDefaultOffAndCapturedAtConstruction) {
  for (const char *value : {static_cast<const char *>(nullptr), "", "0", "true", "1"}) {
    ScopedFlag flag{value};
    td::actor::TestScheduler scheduler;
    scheduler.run([&]() -> td::actor::Task<> {
      auto ring = td::actor::create_actor<InspectableKeyring>("keyring", "");
      auto expected = value && td::Slice(value) == "1";
      ScopedFlag::set(expected ? "0" : "1");
      ASSERT_EQ(co_await td::actor::ask(ring.get(), &InspectableKeyring::prepared_enabled), expected);
      co_return {};
    });
  }
}

TEST(KeyringPreparedSigning, RealKeyringSignExportReloadDeleteAndNonEd25519BothModes) {
  for (const char *value : {"0", "1"}) {
    ScopedFlag flag{value};
    const auto directory = td::mkdtemp(td::get_temporary_dir(), "keyring-prepared-").move_as_ok();
    auto key = fixed_key();
    const auto key_hash = key.compute_short_id();
    auto baseline = key.create_decryptor().move_as_ok();
    td::actor::TestScheduler scheduler;
    scheduler.run([&]() -> td::actor::Task<> {
      auto ring = Keyring::create(directory);
      co_await td::actor::ask(ring.get(), &Keyring::add_key, key, false);
      auto exported = co_await td::actor::ask(ring.get(), &Keyring::export_private_key, key_hash);
      ASSERT_EQ(exported.export_as_slice().as_slice(), key.export_as_slice().as_slice());
      auto public_key = co_await td::actor::ask(ring.get(), &Keyring::get_public_key, key_hash);
      ASSERT_TRUE(public_key.compute_short_id() == key_hash);
      auto signed_with_key = co_await td::actor::ask(ring.get(), &Keyring::sign_add_get_public_key, key_hash,
                                                     td::BufferSlice("scalar"));
      ASSERT_TRUE(signed_with_key.second.compute_short_id() == key_hash);
      ASSERT_EQ(signed_with_key.first.as_slice(), baseline->sign("scalar").move_as_ok().as_slice());
      std::vector<td::BufferSlice> messages;
      messages.emplace_back("");
      messages.emplace_back("domain-A:data");
      messages.emplace_back("domain-B:data");
      auto batch = co_await td::actor::ask(ring.get(), &Keyring::sign_messages, key_hash, std::move(messages));
      ASSERT_EQ(batch.size(), 3u);
      ASSERT_EQ(batch[0].move_as_ok().as_slice(), baseline->sign("").move_as_ok().as_slice());
      ASSERT_EQ(batch[1].move_as_ok().as_slice(), baseline->sign("domain-A:data").move_as_ok().as_slice());
      ASSERT_EQ(batch[2].move_as_ok().as_slice(), baseline->sign("domain-B:data").move_as_ok().as_slice());
      auto cipher = public_key.create_encryptor().move_as_ok()->encrypt("decrypt payload").move_as_ok();
      auto plain = co_await td::actor::ask(ring.get(), &Keyring::decrypt_message, key_hash, std::move(cipher));
      ASSERT_EQ(plain.as_slice(), "decrypt payload");
      ring.reset();
      co_await scheduler.wait_sync_work();
      ring = Keyring::create(directory);
      auto reloaded = co_await td::actor::ask(ring.get(), &Keyring::sign_message, key_hash, td::BufferSlice("reload"));
      ASSERT_EQ(reloaded.as_slice(), baseline->sign("reload").move_as_ok().as_slice());
      co_await td::actor::ask(ring.get(), &Keyring::del_key, key_hash);
      auto missing = co_await td::actor::ask(ring.get(), &Keyring::sign_message, key_hash, td::BufferSlice("deleted")).wrap();
      ASSERT_TRUE(missing.is_error());
      co_await td::actor::ask(ring.get(), &Keyring::add_key, key, false);
      auto readded = co_await td::actor::ask(ring.get(), &Keyring::sign_message, key_hash, td::BufferSlice("reload"));
      ASSERT_EQ(readded.as_slice(), reloaded.as_slice());

      PrivateKey unencrypted{privkeys::Unenc{td::Slice("test-unencrypted")}};
      co_await td::actor::ask(ring.get(), &Keyring::add_key, unencrypted, true);
      auto unsigned_result = co_await td::actor::ask(ring.get(), &Keyring::sign_message,
                                                     unencrypted.compute_short_id(), td::BufferSlice("unchanged"));
      ASSERT_EQ(unsigned_result.size(), 0u);
      co_return {};
    });
    td::rmrf(directory).ensure();
  }
}

}  // namespace ton::keyring
