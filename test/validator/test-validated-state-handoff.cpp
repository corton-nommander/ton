#include "td/utils/tests.h"
#include "validator/validated-state-handoff.h"

namespace ton::validator {
namespace {

Bits256 bits(unsigned char value) {
  auto result = Bits256::zero();
  result.as_array()[31] = value;
  return result;
}

BlockIdExt block_id(BlockSeqno seqno, unsigned char root = 0, unsigned char file = 0) {
  return {BlockId{basechainId, shardIdAll, seqno}, bits(root), bits(file)};
}

}  // namespace

TEST(ValidatedStateHandoff, FlagIsStrictAndDefaultOffCompatible) {
  auto disabled = parse_validated_state_handoff_flag("0");
  auto enabled = parse_validated_state_handoff_flag("1");
  ASSERT_TRUE(disabled.has_value());
  ASSERT_TRUE(enabled.has_value());
  ASSERT_TRUE(!*disabled);
  ASSERT_TRUE(*enabled);
  ASSERT_TRUE(!parse_validated_state_handoff_flag("").has_value());
  ASSERT_TRUE(!parse_validated_state_handoff_flag("true").has_value());
  ASSERT_TRUE(!parse_validated_state_handoff_flag("01").has_value());
}

TEST(ValidatedStateHandoff, StateRootIsCheckedAgainstStateUpdateNotBlockRoot) {
  auto block = block_id(42, 1, 2);
  auto state_root = bits(9);
  ASSERT_TRUE(state_root != block.root_hash);
  ASSERT_TRUE(validated_state_handoff_identity_matches(block, block, state_root, state_root));
  ASSERT_TRUE(!validated_state_handoff_identity_matches(block, block, state_root, bits(8)));
  ASSERT_TRUE(!validated_state_handoff_identity_matches(block, block_id(42, 3, 2), state_root, state_root));
}

TEST(ValidatedStateHandoff, RequiresActualBlockRootBeforeCacheLookup) {
  auto block = block_id(42, 1, 2);
  ASSERT_TRUE(validated_state_handoff_block_matches(block, block, block.root_hash));
  ASSERT_TRUE(!validated_state_handoff_block_matches(block_id(42, 3, 2), block, block.root_hash));
  ASSERT_TRUE(!validated_state_handoff_block_matches(block, block_id(42, 1, 4), block.root_hash));
  ASSERT_TRUE(!validated_state_handoff_block_matches(block, block, bits(9)));
}

TEST(ValidatedStateHandoff, RequiresAndConsumesExactBlockIdentity) {
  ValidatedStateHandoffCache<int> cache{4, 10.0};
  auto exact = block_id(42, 1, 2);
  auto other_root = block_id(42, 3, 2);
  auto other_file = block_id(42, 1, 4);
  cache.insert(exact, 17, td::Timestamp::at(100.0));

  ASSERT_TRUE(!cache.take(other_root, td::Timestamp::at(101.0)).has_value());
  ASSERT_TRUE(!cache.take(other_file, td::Timestamp::at(101.0)).has_value());
  auto hit = cache.take(exact, td::Timestamp::at(101.0));
  ASSERT_TRUE(hit.has_value());
  ASSERT_EQ(*hit, 17);
  ASSERT_TRUE(!cache.take(exact, td::Timestamp::at(101.0)).has_value());

  ASSERT_EQ(cache.stats().inserts, 1u);
  ASSERT_EQ(cache.stats().hits, 1u);
  ASSERT_EQ(cache.stats().misses, 3u);
  ASSERT_EQ(cache.stats().evictions, 0u);
}

TEST(ValidatedStateHandoff, EvictsOldestAndExpiresUnusedStates) {
  ValidatedStateHandoffCache<int> bounded{2, 10.0};
  bounded.insert(block_id(1), 1, td::Timestamp::at(100.0));
  bounded.insert(block_id(2), 2, td::Timestamp::at(101.0));
  bounded.insert(block_id(3), 3, td::Timestamp::at(102.0));
  ASSERT_EQ(bounded.size(), 2u);
  ASSERT_TRUE(!bounded.take(block_id(1), td::Timestamp::at(103.0)).has_value());
  auto second = bounded.take(block_id(2), td::Timestamp::at(103.0));
  auto third = bounded.take(block_id(3), td::Timestamp::at(103.0));
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());
  ASSERT_EQ(*second, 2);
  ASSERT_EQ(*third, 3);
  ASSERT_EQ(bounded.stats().evictions, 1u);

  ValidatedStateHandoffCache<int> expiring{2, 10.0};
  expiring.insert(block_id(4), 4, td::Timestamp::at(200.0));
  expiring.expire(td::Timestamp::at(209.999));
  ASSERT_EQ(expiring.size(), 1u);
  ASSERT_TRUE(!expiring.take(block_id(4), td::Timestamp::at(210.0)).has_value());
  ASSERT_EQ(expiring.size(), 0u);
  ASSERT_EQ(expiring.stats().evictions, 0u);
  ASSERT_EQ(expiring.stats().expirations, 1u);
  ASSERT_EQ(expiring.stats().misses, 1u);
}

}  // namespace ton::validator
