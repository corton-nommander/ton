#include <array>
#include <future>
#include <thread>
#include <vector>

#include "td/utils/tests.h"
#include "validator/impl/native-pool-owner-coordination.h"

namespace ton::validator {
namespace {

StdSmcAddress address(unsigned lane = 0, unsigned suffix = 0) {
  auto result = StdSmcAddress::zero();
  result.as_array()[0] = static_cast<unsigned char>(lane << 6);
  result.as_array()[31] = static_cast<unsigned char>(suffix);
  return result;
}

using Coordination = NativePoolOwnerCoordination;
using Permit = std::unique_ptr<Coordination::Permit>;

}  // namespace

// Deterministic local timestamps exercise the same counter implementation;
// these tests never move the process-global clock or run actor schedulers.
class NativePreparedOwnerCoordinationTestAccess {
 public:
  static void initialize_clock(Coordination &owner, double now) {
    owner.cleanup_at_ = td::Timestamp::at(now);
  }
  static std::size_t query(Coordination &owner, WorkchainId wc, const StdSmcAddress &addr, bool increment,
                           double now) {
    return owner.checked_message_count(wc, addr, increment, td::Timestamp::at(now));
  }
  static double cleanup_at(const Coordination &owner) { return owner.cleanup_at_.at(); }
};

TEST(NativePreparedOwnerCoordination, FourOwnersRouteExactPrefixes) {
  Coordination owner(4, 16);
  ASSERT_EQ(owner.owners, 4u);
  ASSERT_EQ(owner.prefix_bits, 2u);
  ASSERT_EQ(owner.published_generation.load(), 0u);
  ASSERT_TRUE(!owner.topology_ready.load());
  for (unsigned lane = 0; lane < 4; ++lane) {
    auto leaf = ShardIdFull{basechainId, (static_cast<ShardId>(lane) * 2 + 1) << 61};
    ASSERT_EQ(owner.owner_for(basechainId, address(lane)), lane);
    ASSERT_EQ(owner.owner_for(leaf), lane);
    ASSERT_TRUE(owner.contains_leaf(leaf));
    ASSERT_EQ(owner.owner_for(masterchainId, address(lane)), 0u);
  }
  ASSERT_TRUE(!owner.contains_leaf({basechainId, shardIdAll}));
  ASSERT_TRUE(!owner.contains_leaf({basechainId, 0x4000000000000000ull}));
  ASSERT_TRUE(owner.contains_leaf({masterchainId, shardIdAll}));
}

TEST(NativePreparedOwnerCoordination, AggregatePriorityQuotaAndPromotionRetainOriginal) {
  auto owner = std::make_shared<Coordination>(4, 2);
  auto old = owner->try_acquire(owner, 0, basechainId, address());
  auto target1 = owner->try_acquire(owner, 1, basechainId, address(1));
  auto target2 = owner->try_acquire(owner, 1, basechainId, address(2));
  ASSERT_TRUE(old && target1 && target2);
  auto rejected = owner->try_acquire(owner, 1, basechainId, address());
  ASSERT_TRUE(!rejected);
  ASSERT_EQ(owner->count(0), 1u);
  ASSERT_EQ(owner->count(1), 2u);
  ASSERT_EQ(owner->address_count(0, basechainId, address()), 1u);
  target1.reset();
  auto promoted = owner->try_acquire(owner, 1, basechainId, address());
  ASSERT_TRUE(promoted != nullptr);
  ASSERT_EQ(owner->count(), 3u);
  old.reset();
  ASSERT_EQ(owner->count(0), 0u);
  ASSERT_EQ(owner->address_count(0, basechainId, address()), 0u);
  ASSERT_EQ(owner->count(), 2u);
  target2.reset();
  promoted.reset();
  ASSERT_EQ(owner->count(), 0u);
}

TEST(NativePreparedOwnerCoordination, LimitReductionPreservesMembershipAndFailureRefunds) {
  auto owner = std::make_shared<Coordination>(4, 3);
  auto a = owner->try_acquire(owner, 0, basechainId, address());
  auto b = owner->try_acquire(owner, 0, basechainId, address());
  auto c = owner->try_acquire(owner, 0, basechainId, address(1));
  owner->set_limit(1);
  ASSERT_EQ(owner->limit(), 1u);
  ASSERT_EQ(owner->count(), 3u);
  ASSERT_TRUE(!owner->try_acquire(owner, 0, basechainId, address(2)));
  a.reset();
  b.reset();
  ASSERT_TRUE(!owner->try_acquire(owner, 0, basechainId, address(2)));
  c.reset();
  {
    auto rollback = owner->try_acquire(owner, 0, basechainId, address(2));
    ASSERT_TRUE(rollback != nullptr);
  }
  ASSERT_EQ(owner->count(), 0u);
  ASSERT_EQ(owner->address_count(0, basechainId, address(2)), 0u);
  std::weak_ptr<Coordination> lifetime = owner;
  auto membership = owner->try_acquire(owner, 0, basechainId, address());
  owner.reset();
  ASSERT_TRUE(!lifetime.expired());
  membership.reset();
  ASSERT_TRUE(lifetime.expired());
}

TEST(NativePreparedOwnerCoordination, GenericAndNativeShareAddressCapAcrossConcurrentOwners) {
  auto owner = std::make_shared<Coordination>(4, 20000);
  std::array<std::vector<Permit>, 4> memberships;
  std::array<std::thread, 4> threads;
  for (unsigned actor = 0; actor < 4; ++actor) {
    threads[actor] = std::thread([&, actor] {
      for (unsigned i = 0; i < 3000; ++i) {
        auto permit = owner->try_acquire(owner, 0, basechainId, address());
        if (permit) { memberships[actor].push_back(std::move(permit)); }
      }
    });
  }
  for (auto &thread : threads) { thread.join(); }
  ASSERT_EQ(owner->count(0), Coordination::PER_ADDRESS_LIMIT);
  ASSERT_EQ(owner->address_count(0, basechainId, address()), Coordination::PER_ADDRESS_LIMIT);
  ASSERT_TRUE(!owner->try_acquire(owner, 0, basechainId, address()));
  // Priority, workchain and address are each part of the membership key.
  auto other_priority = owner->try_acquire(owner, 1, basechainId, address());
  auto other_workchain = owner->try_acquire(owner, 0, masterchainId, address());
  auto other_address = owner->try_acquire(owner, 0, basechainId, address(0, 1));
  ASSERT_TRUE(other_priority && other_workchain && other_address);
  for (auto &actor_memberships : memberships) { actor_memberships.clear(); }
  ASSERT_EQ(owner->address_count(0, basechainId, address()), 0u);
  ASSERT_EQ(owner->count(), 3u);
  auto replacement = owner->try_acquire(owner, 0, basechainId, address());
  ASSERT_TRUE(replacement != nullptr);
}

TEST(NativePreparedOwnerCoordination, ReentrantReadFenceBlocksGenerationPublication) {
  Coordination owner(4, 16), other(4, 16);
  owner.topology_ready.store(true);
  std::promise<bool> writer_blocked;
  auto blocked = writer_blocked.get_future();
  std::thread writer;
  {
    Coordination::ReadFence outer(&owner);
    writer = std::thread([&] {
      std::unique_lock try_writer(owner.state_fence, std::try_to_lock);
      bool blocked_by_reader = !try_writer.owns_lock();
      if (try_writer.owns_lock()) { try_writer.unlock(); }
      writer_blocked.set_value(blocked_by_reader);
      std::unique_lock lock(owner.state_fence);
      owner.published_generation.store(1);
    });
    ASSERT_TRUE(blocked.get());
    {
      Coordination::ReadFence unrelated(&other);
      Coordination::ReadFence nested(&owner);
      ASSERT_TRUE(Coordination::ReadFence::held_on_this_thread(&owner));
      ASSERT_TRUE(Coordination::ReadFence::held_on_this_thread(&other));
      ASSERT_EQ(owner.published_generation.load(), 0u);
    }
    ASSERT_TRUE(Coordination::ReadFence::held_on_this_thread(&owner));
    ASSERT_TRUE(!Coordination::ReadFence::held_on_this_thread(&other));
  }
  writer.join();
  ASSERT_TRUE(!Coordination::ReadFence::held_on_this_thread(&owner));
  Coordination::ReadFence commit(&owner);
  td::uint64 applied_generation = 0;
  ASSERT_TRUE(applied_generation != owner.published_generation.load());
  applied_generation = 1;
  ASSERT_EQ(applied_generation, owner.published_generation.load());
}

TEST(NativePreparedOwnerCoordination, CheckedMessageCounterPreservesTwoHalfWindow) {
  Coordination owner(4, 16);
  using Access = NativePreparedOwnerCoordinationTestAccess;
  Access::initialize_clock(owner, 100);
  auto query = [&](bool increment, double now) { return Access::query(owner, basechainId, address(), increment, now); };
  ASSERT_EQ(query(false, 100), 0u);
  ASSERT_EQ(query(true, 100), 1u);
  ASSERT_EQ(query(true, 104), 2u);
  ASSERT_EQ(query(false, 104.999), 2u);
  ASSERT_EQ(query(false, 105), 2u);
  ASSERT_EQ(query(true, 105), 3u);
  ASSERT_EQ(query(false, 109.999), 3u);
  ASSERT_EQ(query(false, 110), 1u);
  ASSERT_EQ(query(false, 115), 0u);
  ASSERT_EQ(query(true, 115), 1u);
  ASSERT_EQ(query(false, 1000), 0u);
  ASSERT_EQ(Access::cleanup_at(owner), 1005.0);
  ASSERT_EQ(Access::query(owner, masterchainId, address(), true, 1000), 1u);
  ASSERT_EQ(query(false, 1000), 0u);
}

TEST(NativePreparedOwnerCoordination, CheckedMessageCounterSharesConcurrentGenericAndNativeIncrements) {
  Coordination owner(4, 16);
  using Access = NativePreparedOwnerCoordinationTestAccess;
  Access::initialize_clock(owner, 100);
  std::array<std::thread, 4> threads;
  for (auto &thread : threads) {
    thread = std::thread([&] {
      for (unsigned i = 0; i < 1000; ++i) {
        Access::query(owner, basechainId, address(), true, 100);
      }
    });
  }
  for (auto &thread : threads) { thread.join(); }
  ASSERT_EQ(Access::query(owner, basechainId, address(), false, 100), 4000u);
  ASSERT_EQ(Access::query(owner, basechainId, address(), false, 105), 4000u);
  ASSERT_EQ(Access::query(owner, basechainId, address(), false, 110), 0u);
}

}  // namespace ton::validator
