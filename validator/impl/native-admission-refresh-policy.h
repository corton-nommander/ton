#pragma once

#include <optional>

#include "td/utils/Time.h"
#include "ton/ton-types.h"

namespace ton::validator {

// Sequence numbers do not distinguish competing blocks, and a full block ID
// alone does not detect an inconsistent state-root projection.
struct NativeAdmissionStateIdentity {
  BlockIdExt block_id;
  RootHash state_root;

  bool operator==(const NativeAdmissionStateIdentity &other) const {
    return block_id == other.block_id && state_root == other.state_root;
  }
};

enum class NativeAdmissionRefreshDecision { unchanged, refresh, reject_changed, deadline_expired };

// One instance belongs to one original admission call. Refreshing changes the
// state used for full revalidation; it never renews the caller's deadline or
// grants permission to reuse nonce, balance, topology, or reservation checks.
class NativeAdmissionRefreshPolicy {
 public:
  explicit NativeAdmissionRefreshPolicy(td::Timestamp deadline, bool enabled = true)
      : deadline_(deadline), enabled_(enabled) {
  }

  NativeAdmissionRefreshDecision check(const NativeAdmissionStateIdentity &pinned,
                                       const NativeAdmissionStateIdentity &current,
                                       td::Timestamp now = td::Timestamp::now()) {
    if (deadline_ && deadline_.is_in_past(now)) {
      return NativeAdmissionRefreshDecision::deadline_expired;
    }
    if (pinned == current) {
      return NativeAdmissionRefreshDecision::unchanged;
    }
    if (!enabled_ || refreshed_) {
      return NativeAdmissionRefreshDecision::reject_changed;
    }
    refreshed_ = true;
    return NativeAdmissionRefreshDecision::refresh;
  }

  td::Timestamp deadline() const {
    return deadline_;
  }
  bool refreshed() const {
    return refreshed_;
  }

 private:
  const td::Timestamp deadline_;
  const bool enabled_;
  bool refreshed_{false};
};

// This is a request-local proof of successful signature verification, not a
// state-admission cache. The external cell hash binds every field and the
// signature; source is the Ed25519 public key in the native wire format.
struct NativeAdmissionSignatureKey {
  Bits256 message_hash;
  StdSmcAddress source;
  Bits256 chain_domain;

  bool operator==(const NativeAdmissionSignatureKey &other) const {
    return message_hash == other.message_hash && source == other.source && chain_domain == other.chain_domain;
  }
};

class NativeAdmissionSignatureProof {
 public:
  bool reusable(const NativeAdmissionSignatureKey &key) const {
    return verified_ && *verified_ == key;
  }

  void record_result(const NativeAdmissionSignatureKey &key, bool verified) {
    if (verified) {
      verified_ = key;
    } else {
      verified_.reset();
    }
  }

 private:
  std::optional<NativeAdmissionSignatureKey> verified_;
};

}  // namespace ton::validator
