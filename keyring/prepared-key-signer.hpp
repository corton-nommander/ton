#pragma once

#include "crypto/Ed25519.h"
#include "keys/encryptor.h"

namespace ton::keyring {

// Owned by one keyring signing actor. The EVP key is immutable after a successful
// import; the existing sign overload allocates a fresh EVP_MD_CTX for every call.
// Keep this opt-in wrapper outside the general key/client signing path.
class PreparedKeySigner final : public Decryptor {
 public:
  PreparedKeySigner(td::Ed25519::PrivateKey key, std::unique_ptr<Decryptor> decryptor)
      : key_(std::move(key)), decryptor_(std::move(decryptor)) {
  }

  td::Result<td::BufferSlice> decrypt(td::Slice data) override {
    return decryptor_->decrypt(data);
  }

  td::Result<td::BufferSlice> sign(td::Slice data) override {
    if (!prepared_) {
      // Do not retain an import error: a later request must be able to retry,
      // just as it can with DecryptorEd25519::sign's per-call import.
      TRY_RESULT_PREFIX(prepared, key_.prepare(), "failed to sign: ");
      prepared_ = std::move(prepared);
    }
    TRY_RESULT_PREFIX(signature, td::Ed25519::PrivateKey::sign(*prepared_, data), "failed to sign: ");
    return td::BufferSlice(signature);
  }

 private:
  friend class PreparedKeySignerTestAccess;
  td::Ed25519::PrivateKey key_;
  std::unique_ptr<Decryptor> decryptor_;
  std::shared_ptr<const td::Ed25519::PreparedPrivateKey> prepared_;
};

}  // namespace ton::keyring
