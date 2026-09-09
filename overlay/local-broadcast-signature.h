#pragma once

#include <utility>

#include "keys/keys.hpp"
#include "td/utils/Status.h"
#include "td/utils/buffer.h"

namespace ton::overlay {

class BroadcastsSimple;
class BroadcastsFec;
struct LocalBroadcastSignatureTestAccess;

// Process-local evidence owned by one outgoing broadcast/signing callback.
// Neither a wire flag, a local source ID nor a zero peer ID can create it.
class LocalBroadcastSignature {
 public:
  LocalBroadcastSignature(LocalBroadcastSignature &&) = default;
  LocalBroadcastSignature &operator=(LocalBroadcastSignature &&) = default;

  void complete(const td::Result<std::pair<td::BufferSlice, PublicKey>> &result) {
    if (completion_seen_) {
      return;
    }
    completion_seen_ = true;
    if (result.is_error() || !result.ok().second.is_ed25519() || result.ok().first.size() != 64 ||
        result.ok().second.compute_short_id() != requested_signer_) {
      return;
    }
    key_ = result.ok().second;
    // clone() shares mutable storage. The receipt must survive subsequent
    // mutations to the installed message/result without changing its evidence.
    signature_ = result.ok().first.copy();
    succeeded_ = true;
  }

  bool matches(const PublicKey &key, td::Slice message, td::Slice signature) const {
    return succeeded_ && key == key_ && message == message_.as_slice() && signature == signature_.as_slice();
  }

 private:
  friend class BroadcastsSimple;
  friend class BroadcastsFec;
  friend struct LocalBroadcastSignatureTestAccess;

  LocalBroadcastSignature(PublicKeyHash requested_signer, td::Slice message)
      : requested_signer_(requested_signer), message_(message) {
  }

  PublicKeyHash requested_signer_;
  td::BufferSlice message_;
  PublicKey key_;
  td::BufferSlice signature_;
  bool completion_seen_{false};
  bool succeeded_{false};
};

}  // namespace ton::overlay
