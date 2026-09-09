#include "td/utils/tests.h"
#include "validator/impl/native-admission-refresh-policy.h"

namespace {

using ton::validator::NativeAdmissionRefreshDecision;
using ton::validator::NativeAdmissionRefreshPolicy;
using ton::validator::NativeAdmissionSignatureKey;
using ton::validator::NativeAdmissionSignatureProof;
using ton::validator::NativeAdmissionStateIdentity;

ton::Bits256 hash(unsigned char suffix) {
  auto value = ton::Bits256::zero();
  value.as_array()[31] = suffix;
  return value;
}

NativeAdmissionStateIdentity state(ton::BlockSeqno seqno, unsigned char root = 1, unsigned char file = 1,
                                   unsigned char state_root = 1) {
  return {ton::BlockIdExt{ton::BlockId{ton::masterchainId, ton::shardIdAll, seqno}, hash(root), hash(file)},
          hash(state_root)};
}

void expect(NativeAdmissionRefreshDecision actual, NativeAdmissionRefreshDecision expected) {
  ASSERT_EQ(static_cast<unsigned>(actual), static_cast<unsigned>(expected));
}

}  // namespace

TEST(NativeAdmissionRefresh, OneChangedSnapshotRefreshesWithoutRenewingDeadline) {
  NativeAdmissionRefreshPolicy policy(td::Timestamp::at(200));
  expect(policy.check(state(10), state(10), td::Timestamp::at(100)), NativeAdmissionRefreshDecision::unchanged);
  ASSERT_TRUE(!policy.refreshed());
  expect(policy.check(state(10), state(11), td::Timestamp::at(110)), NativeAdmissionRefreshDecision::refresh);
  ASSERT_TRUE(policy.refreshed());
  ASSERT_EQ(policy.deadline().at(), 200.0);
  expect(policy.check(state(11), state(11), td::Timestamp::at(150)), NativeAdmissionRefreshDecision::unchanged);
  expect(policy.check(state(11), state(12), td::Timestamp::at(160)), NativeAdmissionRefreshDecision::reject_changed);
  ASSERT_EQ(policy.deadline().at(), 200.0);
}

TEST(NativeAdmissionRefresh, DeadlineWinsEvenWhenSnapshotIsUnchanged) {
  NativeAdmissionRefreshPolicy policy(td::Timestamp::at(200));
  expect(policy.check(state(10), state(10), td::Timestamp::at(200)),
         NativeAdmissionRefreshDecision::deadline_expired);
  expect(policy.check(state(10), state(11), td::Timestamp::at(201)),
         NativeAdmissionRefreshDecision::deadline_expired);
  ASSERT_TRUE(!policy.refreshed());
}

TEST(NativeAdmissionRefresh, RefreshedAttemptCannotContinueAtOriginalDeadline) {
  NativeAdmissionRefreshPolicy policy(td::Timestamp::at(200));
  expect(policy.check(state(10), state(11), td::Timestamp::at(199)), NativeAdmissionRefreshDecision::refresh);
  expect(policy.check(state(11), state(11), td::Timestamp::at(200)),
         NativeAdmissionRefreshDecision::deadline_expired);
  expect(policy.check(state(11), state(12), td::Timestamp::at(201)),
         NativeAdmissionRefreshDecision::deadline_expired);
}

TEST(NativeAdmissionRefresh, FullBlockAndStateRootIdentityAreRequired) {
  const auto original = state(10);
  for (const auto &changed : {state(11), state(10, 2), state(10, 1, 2), state(10, 1, 1, 2)}) {
    NativeAdmissionRefreshPolicy policy(td::Timestamp::at(200));
    expect(policy.check(original, changed, td::Timestamp::at(100)), NativeAdmissionRefreshDecision::refresh);
  }
}

TEST(NativeAdmissionRefresh, DisabledControlKeepsSnapshotRejection) {
  NativeAdmissionRefreshPolicy policy(td::Timestamp::at(200), false);
  expect(policy.check(state(10), state(10), td::Timestamp::at(100)), NativeAdmissionRefreshDecision::unchanged);
  expect(policy.check(state(10), state(11), td::Timestamp::at(100)), NativeAdmissionRefreshDecision::reject_changed);
  ASSERT_TRUE(!policy.refreshed());
}

TEST(NativeAdmissionRefresh, UnboundedDeadlineStillAllowsOnlyOneRefresh) {
  NativeAdmissionRefreshPolicy policy(td::Timestamp::never());
  expect(policy.check(state(10), state(11), td::Timestamp::at(1e9)), NativeAdmissionRefreshDecision::refresh);
  expect(policy.check(state(11), state(12), td::Timestamp::at(1e9 + 1)),
         NativeAdmissionRefreshDecision::reject_changed);
  ASSERT_TRUE(!policy.deadline());
}

TEST(NativeAdmissionRefresh, SignatureReuseRequiresSuccessAndExactMessageSourceDomain) {
  const NativeAdmissionSignatureKey key{hash(1), hash(2), hash(3)};
  NativeAdmissionSignatureProof proof;
  ASSERT_TRUE(!proof.reusable(key));
  proof.record_result(key, false);
  ASSERT_TRUE(!proof.reusable(key));
  proof.record_result(key, true);
  ASSERT_TRUE(proof.reusable(key));
  ASSERT_TRUE(!proof.reusable({hash(4), hash(2), hash(3)}));
  ASSERT_TRUE(!proof.reusable({hash(1), hash(4), hash(3)}));
  ASSERT_TRUE(!proof.reusable({hash(1), hash(2), hash(4)}));
  ASSERT_TRUE(proof.reusable(key));
}

TEST(NativeAdmissionRefresh, FailedVerificationDiscardsOlderProof) {
  const NativeAdmissionSignatureKey original{hash(1), hash(2), hash(3)};
  const NativeAdmissionSignatureKey changed_domain{hash(1), hash(2), hash(4)};
  NativeAdmissionSignatureProof proof;
  proof.record_result(original, true);
  proof.record_result(changed_domain, false);
  ASSERT_TRUE(!proof.reusable(original));
  ASSERT_TRUE(!proof.reusable(changed_domain));
  proof.record_result(changed_domain, true);
  ASSERT_TRUE(!proof.reusable(original));
  ASSERT_TRUE(proof.reusable(changed_domain));
}
