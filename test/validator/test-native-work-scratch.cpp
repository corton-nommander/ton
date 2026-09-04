#include <map>
#include <vector>

#include "td/utils/tests.h"
#include "validator/impl/native-work-scratch.h"

namespace {

struct Snapshot {
  td::uint64 balance{0};
  td::uint64 nonce{0};
  bool changed{false};
};

ton::StdSmcAddress address(td::uint64 prefix) {
  auto result = ton::StdSmcAddress::zero();
  result.bits().store_uint(prefix, 64);
  return result;
}

using Scratch = ton::validator::detail::NativeWorkScratch<Snapshot, 3>;

}  // namespace

TEST(NativeWorkScratch, DeduplicatesAndPreservesSortedOrder) {
  Scratch scratch;
  ASSERT_TRUE(scratch.add_address(address(30)));
  ASSERT_TRUE(scratch.add_address(address(10)));
  ASSERT_TRUE(scratch.add_address(address(20)));
  ASSERT_TRUE(scratch.add_address(address(10)));
  ASSERT_EQ(scratch.size(), 3u);

  std::vector<ton::StdSmcAddress> addresses;
  scratch.for_each_address([&](const auto& value) { addresses.push_back(value); });
  ASSERT_EQ(addresses.size(), 3u);
  ASSERT_EQ(addresses[0], address(10));
  ASSERT_EQ(addresses[1], address(20));
  ASSERT_EQ(addresses[2], address(30));
}

TEST(NativeWorkScratch, CapacityFailureIsNonMutating) {
  Scratch scratch;
  ASSERT_TRUE(scratch.add_address(address(10)));
  ASSERT_TRUE(scratch.add_address(address(20)));
  ASSERT_TRUE(scratch.add_address(address(30)));
  ASSERT_TRUE(scratch.add_address(address(20)));
  ASSERT_TRUE(!scratch.add_address(address(40)));
  ASSERT_EQ(scratch.size(), Scratch::capacity());
  ASSERT_TRUE(scratch.contains(address(10)));
  ASSERT_TRUE(scratch.contains(address(20)));
  ASSERT_TRUE(scratch.contains(address(30)));
  ASSERT_TRUE(!scratch.contains(address(40)));
  ASSERT_TRUE(!scratch.capture_before(address(40), Snapshot{.balance = 40}));
}

TEST(NativeWorkScratch, FirstSnapshotWinsForRollback) {
  Scratch scratch;
  ASSERT_TRUE(scratch.add_address(address(20)));
  ASSERT_TRUE(scratch.add_address(address(10)));
  ASSERT_TRUE(scratch.add_address(address(30)));
  ASSERT_TRUE(scratch.capture_before(address(20), Snapshot{.balance = 200, .nonce = 2, .changed = false}));
  ASSERT_TRUE(scratch.capture_before(address(20), Snapshot{.balance = 999, .nonce = 9, .changed = true}));
  ASSERT_TRUE(scratch.capture_before(address(10), Snapshot{.balance = 100, .nonce = 1, .changed = true}));

  std::map<ton::StdSmcAddress, Snapshot> rollback;
  scratch.for_each_snapshot([&](const auto& key, const auto& snapshot) { rollback.emplace(key, snapshot); });
  ASSERT_EQ(rollback.size(), 2u);
  ASSERT_EQ(rollback.at(address(10)).balance, 100u);
  ASSERT_EQ(rollback.at(address(20)).balance, 200u);
  ASSERT_EQ(rollback.at(address(20)).nonce, 2u);
  ASSERT_TRUE(!rollback.at(address(20)).changed);
}

TEST(NativeWorkScratch, ClearReleasesSnapshotsAndAllowsReuse) {
  Scratch scratch;
  ASSERT_TRUE(scratch.add_address(address(10)));
  ASSERT_TRUE(scratch.capture_before(address(10), Snapshot{.balance = 100}));
  scratch.clear();
  ASSERT_TRUE(scratch.empty());
  ASSERT_TRUE(!scratch.contains(address(10)));

  std::size_t snapshots = 0;
  scratch.for_each_snapshot([&](const auto&, const auto&) { ++snapshots; });
  ASSERT_EQ(snapshots, 0u);
  ASSERT_TRUE(scratch.add_address(address(40)));
  ASSERT_TRUE(scratch.capture_before(address(40), Snapshot{.balance = 400}));
  scratch.for_each_snapshot([&](const auto&, const auto& snapshot) {
    ++snapshots;
    ASSERT_EQ(snapshot.balance, 400u);
  });
  ASSERT_EQ(snapshots, 1u);
}

TEST(NativeWorkScratch, FragmentPromotionKeepsEarliestSnapshot) {
  Scratch scratch;
  std::map<ton::StdSmcAddress, Snapshot> fragment_journal;

  ASSERT_TRUE(scratch.add_address(address(10)));
  ASSERT_TRUE(scratch.capture_before(address(10), Snapshot{.balance = 100}));
  scratch.for_each_snapshot(
      [&](const auto& key, const auto& snapshot) { fragment_journal.try_emplace(key, snapshot); });

  scratch.clear();
  ASSERT_TRUE(scratch.add_address(address(10)));
  ASSERT_TRUE(scratch.capture_before(address(10), Snapshot{.balance = 50}));
  scratch.for_each_snapshot(
      [&](const auto& key, const auto& snapshot) { fragment_journal.try_emplace(key, snapshot); });

  ASSERT_EQ(fragment_journal.size(), 1u);
  ASSERT_EQ(fragment_journal.at(address(10)).balance, 100u);
}
