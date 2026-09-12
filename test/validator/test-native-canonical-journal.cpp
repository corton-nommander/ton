#include <limits>
#include <sstream>

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/mc-config.h"
#include "td/utils/tests.h"
#include "tl/tlblib.hpp"
#include "validator/impl/ext-message-pool.hpp"
#include "validator/impl/shard.hpp"
#include "vm/dict.h"

namespace ton::validator {
namespace {

Bits256 bits(unsigned value) {
  auto result = Bits256::zero();
  result.as_array()[30] = static_cast<unsigned char>(value >> 8);
  result.as_array()[31] = static_cast<unsigned char>(value);
  return result;
}

BlockIdExt block_id(WorkchainId wc, BlockSeqno seqno, unsigned revision = 0) {
  return {BlockId{wc, shardIdAll, seqno}, bits(seqno + revision * 1000), bits(100 + revision)};
}

struct AccountFacts {
  td::uint64 balance{100};
  td::uint64 nonce{0};
};

class JournalShardState final : public ShardStateQ {
 public:
  JournalShardState(BlockSeqno seqno, const std::map<unsigned, AccountFacts> &facts,
                    UnixTime utime = 0, LogicalTime lt = 0, unsigned revision = 0)
      : ShardStateQ(block_id(basechainId, seqno, revision), make_root(seqno, facts, utime, lt)) {
  }
  RootHash root_hash() const override {
    return RootHash{root_cell()->get_hash().bits()};
  }

 private:
  static td::Ref<vm::Cell> make_root(BlockSeqno seqno, const std::map<unsigned, AccountFacts> &facts,
                                    UnixTime utime, LogicalTime lt) {
    vm::AugmentedDictionary accounts{256, block::tlb::aug_ShardAccounts};
    for (const auto &[key, value] : facts) {
      auto account = vm::CellBuilder{}.store_long(1, 2).store_long(value.balance, 64)
          .store_long(value.nonce, 64).store_long(0, 8).finalize_novm();
      auto shard_account = vm::CellBuilder{}.store_ref(account).store_zeroes(256).store_long(0, 64).finalize_novm();
      CHECK(accounts.set(bits(key), vm::load_cell_slice_ref(shard_account)));
    }
    auto empty = vm::CellBuilder{}.finalize_novm();
    auto auxiliary = vm::CellBuilder{}.store_zeroes(140).finalize_novm();
    return vm::CellBuilder{}.store_long(0x9023afe2, 32).store_long(0, 32)
        .store_long(0, 8).store_long(basechainId, 32).store_long(0, 64)
        .store_long(seqno, 32).store_long(0, 32).store_long(utime ? utime : 100 + seqno, 32)
        .store_long(lt ? lt : seqno * 100, 64).store_long(0, 32).store_ref(empty)
        .store_long(0, 1).store_ref(accounts.get_wrapped_dict_root()).store_ref(auxiliary)
        .store_long(0, 1).finalize_novm();
  }
};

class JournalMasterchainState final : public MasterchainStateQ {
 public:
  JournalMasterchainState(BlockSeqno seqno, BlockIdExt shard, unsigned revision = 0)
      : MasterchainStateQ(block_id(masterchainId, seqno, revision),
                          vm::CellBuilder{}.store_long(seqno, 32).store_long(revision, 32).finalize_novm()),
        shard_(td::make_ref<block::McShardHash>(shard, 0, seqno * 100)) {
  }
  td::Ref<McShardHash> get_shard_from_config(ShardIdFull, bool) const override {
    return shard_;
  }
 private:
  td::Ref<McShardHash> shard_;
};

class JournalMessage final : public ExtMessage {
 public:
  JournalMessage(StdSmcAddress address, Hash hash) : address_(address), hash_(hash) {
  }
  AccountIdPrefixFull shard() const override { return extract_addr_prefix(basechainId, address_); }
  td::BufferSlice serialize() const override { return {}; }
  td::Ref<vm::Cell> root_cell() const override { return {}; }
  Hash hash() const override { return hash_; }
  Hash hash_norm() const override { return hash_; }
  WorkchainId wc() const override { return basechainId; }
  StdSmcAddress addr() const override { return address_; }
 private:
  StdSmcAddress address_;
  Hash hash_;
};

}  // namespace

class NativeCanonicalJournalTestAccess {
 public:
  using Address = ExtMessagePool::NativeAddress;
  static ExtMessagePool pool() { return ExtMessagePool({}, {}); }
  static Address address(unsigned key) { return {basechainId, bits(key)}; }
  static std::vector<Address> addresses(std::initializer_list<unsigned> keys) {
    std::vector<Address> result;
    for (auto key : keys) { result.push_back(address(key)); }
    return result;
  }
  static const NativeCanonicalJournalTelemetry &stats(const ExtMessagePool &pool) {
    return pool.native_canonical_journal_telemetry_;
  }
  static bool disabled_by_default(const ExtMessagePool &pool) { return !pool.native_canonical_journal_enabled_; }
  static std::size_t endpoint_count(const ExtMessagePool &pool) { return pool.native_canonical_journal_endpoints_.size(); }
  static BlockSeqno checkpoint(const ExtMessagePool &pool) {
    return pool.native_canonical_journal_endpoints_.at({basechainId, shardIdAll}).shard_block.seqno();
  }
  static void corrupt_old_dictionary(ExtMessagePool &pool) {
    pool.native_canonical_journal_endpoints_.at({basechainId, shardIdAll}).accounts =
        vm::CellBuilder{}.finalize_novm();
  }
  static void lose_successful_checkpoint(ExtMessagePool &pool) {
    pool.native_reconciliation_successful_shard_tops_.clear();
  }
  static void reset_topology(ExtMessagePool &pool) {
    pool.prune_native_journal_endpoints({});
  }
  static td::Status journal(ExtMessagePool &pool, const td::Ref<ShardState> &state,
                             const std::vector<Address> &sources, std::set<Address> &changed,
                             BlockSeqno mc_seqno = 0, unsigned mc_revision = 0,
                             td::optional<BlockIdExt> reference = {}) {
    pool.native_canonical_journal_enabled_ = true;
    JournalMasterchainState mc(mc_seqno ? mc_seqno : state->get_seqno(),
                               reference ? reference.value() : state->get_block_id(), mc_revision);
    auto result = pool.reconcile_native_journal_shard(mc, state, sources, changed);
    if (result.is_ok()) { pool.record_successful_native_shard_reconciliation(state->get_block_id()); }
    return result;
  }
  // Independent oracle follows the original full scanner: lookup and decode
  // every tracked account, then run the unmodified canonical application path.
  static td::Status full_scan(ExtMessagePool &pool, const td::Ref<ShardState> &state,
                              const std::vector<Address> &sources, std::set<Address> &changed) {
    block::gen::ShardStateUnsplit::Record info;
    CHECK(tlb::unpack_cell(state->root_cell(), info));
    vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(info.accounts), 256, block::tlb::aug_ShardAccounts};
    td::Status error;
    for (const auto &source : sources) {
      block::Account account;
      if (!account.unpack(accounts.lookup(source.second), info.gen_utime, false) ||
          account.status != block::Account::acc_uninit || !account.is_native || !account.native_balance_uint64()) {
        error = td::Status::Error("invalid native account");
        continue;
      }
      auto result = pool.apply_reconciled_native_account_state(source, account.native_nonce,
                                                               account.native_balance_uint64().value(),
                                                               info.gen_utime, info.gen_lt);
      if (result.is_error()) { error = result.move_as_error(); }
      else if (result.ok()) { changed.insert(source); }
    }
    return error;
  }
  static void reserve(ExtMessagePool &pool, unsigned key, td::uint64 nonce, td::uint32 count = 16,
                       td::uint64 amount = 16, UnixTime expiry = std::numeric_limits<UnixTime>::max(),
                       bool committed = true, bool stale_revision = false) {
    auto source = address(key);
    auto hash = bits(key * 100 + static_cast<unsigned>(nonce) + 1000);
    auto message = td::make_ref<JournalMessage>(source.second, hash);
    auto mempool = std::make_shared<ExtMessagePool::MempoolMsg>(message);
    mempool->native_nonce = nonce;
    mempool->native_nonce_count = count;
    mempool->native_is_run = true;
    mempool->in_mempool = true;
    mempool->active = true;
    ExtMessagePool::MessageId id{message->shard(), hash};
    auto &messages = pool.ext_msgs_[0];
    messages.ext_messages_ = messages.ext_messages_.insert(id, mempool);
    messages.native_messages_ = messages.native_messages_.insert(
        ExtMessagePool::NativeMessageId{nonce, id.dst, id.hash}, mempool);
    messages.ext_addr_messages_[source].emplace(hash, id);
    pool.ext_messages_hashes_[hash] = {0, id};
    pool.ext_messages_hashes_norm_[hash].insert(ExtMessagePool::NormalizedMessageId{0, id});
    auto &work = pool.native_accounts_[source].messages[nonce];
    work.hash = hash;
    work.source = source;
    work.logical_count = count;
    work.is_run = true;
    work.amount = amount;
    work.valid_until = expiry;
    work.account_revision = pool.native_nonce_watermarks_[source].revision + (stale_revision ? 1 : 0);
    work.committed = committed;
    work.set_mempool_link(mempool, 0, id);
    pool.register_pending_native_reconciliation_targets();
  }
  static td::uint64 next_nonce(const ExtMessagePool &pool, unsigned key) {
    auto source = address(key);
    auto account = pool.native_accounts_.find(source);
    if (account != pool.native_accounts_.end() && !account->second.messages.empty()) {
      const auto &[nonce, work] = *account->second.messages.rbegin();
      return nonce + work.logical_count;
    }
    return pool.native_nonce_watermarks_.at(source).observed_next_nonce;
  }
  static std::string state(const ExtMessagePool &pool) {
    std::ostringstream out;
    for (const auto &[source, watermark] : pool.native_nonce_watermarks_) {
      out << source.second.to_hex() << ':' << watermark.observed_next_nonce << ':'
          << (watermark.observed_balance ? watermark.observed_balance.value() : 0) << ':' << watermark.observed_lt << ':' << watermark.revision << ';';
    }
    for (const auto &[source, account] : pool.native_accounts_) {
      out << source.second.to_hex() << ':' << account.observed_nonce << ':' << account.observed_utime << ':';
      for (const auto &[nonce, work] : account.messages) {
        out << nonce << ',' << work.hash.to_hex() << ',' << work.logical_count << ',' << work.amount << ','
            << work.valid_until << ',' << work.account_revision << ',' << work.committed << ';';
      }
    }
    for (const auto &[hash, id] : pool.ext_messages_hashes_) { out << hash.to_hex() << ':' << id.first << ';'; }
    for (const auto &[source, nonce] : pool.locally_accepted_native_nonces_) {
      out << source.second.to_hex() << ':' << nonce << ';';
    }
    return out.str();
  }
  static void compare(ExtMessagePool &journal_pool, ExtMessagePool &full_pool, const td::Ref<ShardState> &state,
                       const std::vector<Address> &sources, bool success = true, BlockSeqno mc_seqno = 0) {
    std::set<Address> journal_changed, full_changed;
    auto journal_result = journal(journal_pool, state, sources, journal_changed, mc_seqno);
    auto full_result = full_scan(full_pool, state, sources, full_changed);
    ASSERT_EQ(journal_result.is_ok(), success);
    ASSERT_EQ(full_result.is_ok(), success);
    ASSERT_EQ(NativeCanonicalJournalTestAccess::state(journal_pool), NativeCanonicalJournalTestAccess::state(full_pool));
    ASSERT_TRUE(journal_changed == full_changed);
  }
};

using Access = NativeCanonicalJournalTestAccess;

TEST(NativeCanonicalJournal, DefaultsOffAndReusesOnlySuccessfulExactSourceObservations) {
  auto journal = Access::pool();
  auto reference = Access::pool();
  ASSERT_TRUE(Access::disabled_by_default(journal));
  auto first = td::make_ref<JournalShardState>(1, std::map<unsigned, AccountFacts>{{1, {}}, {2, {}}});
  Access::compare(journal, reference, first, Access::addresses({1}));
  for (auto *pool : {&journal, &reference}) { Access::reserve(*pool, 1, 0); }
  // A newly tracked account was not checked at the old endpoint. It must be
  // looked up even though the entire account dictionary is unchanged.
  auto next = td::make_ref<JournalShardState>(2, std::map<unsigned, AccountFacts>{{1, {}}, {2, {}}});
  Access::compare(journal, reference, next, Access::addresses({1, 2}));
  ASSERT_EQ(Access::stats(journal).bootstrap, 1u);
  ASSERT_EQ(Access::stats(journal).lookup_bootstraps, 2u);
  ASSERT_EQ(Access::stats(journal).reused_observations, 1u);
  ASSERT_EQ(Access::stats(journal).unchanged_apply_skips, 1u);
  ASSERT_EQ(Access::stats(journal).retained_sources, 2u);
}

TEST(NativeCanonicalJournal, SkippedBlocksIncludeDestinationBalancesAndAtomicNonceEffects) {
  auto journal = Access::pool();
  auto reference = Access::pool();
  Access::compare(journal, reference, td::make_ref<JournalShardState>(1,
      std::map<unsigned, AccountFacts>{{1, {}}, {2, {}}, {3, {}}}), Access::addresses({1, 2, 3}));
  for (auto *pool : {&journal, &reference}) {
    Access::reserve(*pool, 1, 0);
    Access::reserve(*pool, 1, 16);
    Access::reserve(*pool, 2, 0, 16, 50);
    Access::reserve(*pool, 2, 16, 16, 20);
    Access::reserve(*pool, 3, 0);
  }
  // Account 1 consumed only part of an atomic parent; account 2's balance
  // decreases without a nonce change; account 3 receives an incoming credit.
  Access::compare(journal, reference, td::make_ref<JournalShardState>(7,
      std::map<unsigned, AccountFacts>{{1, {92, 8}}, {2, {60, 0}}, {3, {150, 0}}}), Access::addresses({1, 2, 3}));
  ASSERT_EQ(Access::stats(journal).diff_changed_accounts, 3u);
  ASSERT_EQ(Access::stats(journal).diff_tracked_changed, 3u);
  ASSERT_EQ(Access::stats(journal).lookup_bootstraps, 3u);
  ASSERT_EQ(Access::checkpoint(journal), 7u);
}

TEST(NativeCanonicalJournal, UnchangedRootStillExpiresSuffixAndFencesOlderAdmission) {
  auto journal = Access::pool();
  auto reference = Access::pool();
  Access::compare(journal, reference, td::make_ref<JournalShardState>(1,
      std::map<unsigned, AccountFacts>{{1, {}}, {2, {}}, {3, {}}}), Access::addresses({1, 2, 3}));
  for (auto *pool : {&journal, &reference}) {
    Access::reserve(*pool, 1, 0, 16, 16, 200);
    Access::reserve(*pool, 1, 16, 16, 16, 105);
    Access::reserve(*pool, 1, 32, 16, 16, 200);
    Access::reserve(*pool, 2, 0, 16, 16, 200, false, true);
    Access::reserve(*pool, 2, 16);
    Access::reserve(*pool, 3, 0);
  }
  Access::compare(journal, reference, td::make_ref<JournalShardState>(2,
      std::map<unsigned, AccountFacts>{{1, {}}, {2, {}}, {3, {}}}, 105, 200), Access::addresses({1, 2, 3}));
  ASSERT_EQ(Access::stats(journal).reused_observations, 3u);
  ASSERT_EQ(Access::stats(journal).unchanged_apply_skips, 1u);
  // Both paths advanced observed_lt, including the unchanged fast path.
  Access::compare(journal, reference, td::make_ref<JournalShardState>(3,
      std::map<unsigned, AccountFacts>{{1, {}}, {2, {}}, {3, {}}}, 106, 150), Access::addresses({1, 2, 3}), false);
  ASSERT_EQ(Access::checkpoint(journal), 2u);
}

TEST(NativeCanonicalJournal, DeletedAccountCannotPublishPartialEndpoint) {
  auto journal = Access::pool();
  auto reference = Access::pool();
  Access::compare(journal, reference, td::make_ref<JournalShardState>(1,
      std::map<unsigned, AccountFacts>{{1, {}}, {2, {}}}), Access::addresses({1, 2}));
  Access::compare(journal, reference, td::make_ref<JournalShardState>(2,
      std::map<unsigned, AccountFacts>{{1, {110, 0}}}), Access::addresses({1, 2}), false);
  ASSERT_EQ(Access::checkpoint(journal), 1u);
  // Repeat from the last successful root; already-applied account 1 is
  // idempotent, while the missing source still prevents publication.
  Access::compare(journal, reference, td::make_ref<JournalShardState>(2,
      std::map<unsigned, AccountFacts>{{1, {110, 0}}}), Access::addresses({1, 2}), false);
  ASSERT_EQ(Access::stats(journal).failed_shards, 2u);
  ASSERT_EQ(Access::checkpoint(journal), 1u);
}

TEST(NativeCanonicalJournal, MissingContinuityAndBrokenOldDiffFallBackToExactLookup) {
  auto journal = Access::pool();
  auto reference = Access::pool();
  Access::compare(journal, reference, td::make_ref<JournalShardState>(1,
      std::map<unsigned, AccountFacts>{{1, {}}}), Access::addresses({1}));
  Access::lose_successful_checkpoint(journal);
  Access::compare(journal, reference, td::make_ref<JournalShardState>(2,
      std::map<unsigned, AccountFacts>{{1, {110, 0}}}), Access::addresses({1}));
  ASSERT_EQ(Access::stats(journal).continuity_fallbacks, 1u);
  Access::corrupt_old_dictionary(journal);
  Access::compare(journal, reference, td::make_ref<JournalShardState>(3,
      std::map<unsigned, AccountFacts>{{1, {120, 0}}}), Access::addresses({1}));
  ASSERT_EQ(Access::stats(journal).diff_fallbacks, 1u);
  ASSERT_EQ(Access::stats(journal).lookup_bootstraps, 3u);
  Access::reset_topology(journal);
  ASSERT_EQ(Access::endpoint_count(journal), 0u);
  ASSERT_EQ(Access::stats(journal).retained_sources, 0u);
  Access::compare(journal, reference, td::make_ref<JournalShardState>(4,
      std::map<unsigned, AccountFacts>{{1, {120, 0}}}), Access::addresses({1}));
  ASSERT_EQ(Access::stats(journal).bootstrap, 2u);
}

TEST(NativeCanonicalJournal, WrongCanonicalReferenceCannotMutateWatermarksOrCheckpoint) {
  auto journal = Access::pool();
  auto reference = Access::pool();
  Access::compare(journal, reference, td::make_ref<JournalShardState>(1,
      std::map<unsigned, AccountFacts>{{1, {}}}), Access::addresses({1}));
  const auto before = Access::state(journal);
  std::set<Access::Address> changed;
  auto result = Access::journal(journal, td::make_ref<JournalShardState>(2,
      std::map<unsigned, AccountFacts>{{1, {50, 50}}}), Access::addresses({1}), changed, 2, 0,
      block_id(basechainId, 2, 1));
  ASSERT_TRUE(result.is_error());
  ASSERT_EQ(Access::state(journal), before);
  ASSERT_EQ(Access::checkpoint(journal), 1u);
  ASSERT_TRUE(changed.empty());
}

TEST(NativeCanonicalJournal, RepeatedEndpointAndSameHeightIdentityChangeNeverReuseWrongRoot) {
  auto journal = Access::pool();
  auto reference = Access::pool();
  auto first = td::make_ref<JournalShardState>(1, std::map<unsigned, AccountFacts>{{1, {}}});
  Access::compare(journal, reference, first, Access::addresses({1}));
  Access::compare(journal, reference, first, Access::addresses({1}));
  Access::compare(journal, reference, td::make_ref<JournalShardState>(1,
      std::map<unsigned, AccountFacts>{{1, {120, 0}}}, 101, 100, 1), Access::addresses({1}), true, 2);
  ASSERT_EQ(Access::stats(journal).continuity_fallbacks, 1u);
  ASSERT_EQ(Access::stats(journal).lookup_bootstraps, 2u);
}

TEST(NativeCanonicalJournal, RepeatedMixedChangesMatchFullScanFor256Accounts) {
  auto journal = Access::pool();
  auto reference = Access::pool();
  std::map<unsigned, AccountFacts> facts;
  std::vector<Access::Address> sources;
  for (unsigned key = 1; key <= 256; ++key) {
    facts[key] = {10000, 0};
    sources.push_back(Access::address(key));
  }
  Access::compare(journal, reference, td::make_ref<JournalShardState>(1, facts), sources);
  for (unsigned round = 1; round <= 32; ++round) {
    for (unsigned key = 1; key <= 256; ++key) {
      if ((key + round) % 3 == 0) {
        const auto nonce = Access::next_nonce(journal, key);
        for (auto *pool : {&journal, &reference}) {
          Access::reserve(*pool, key, nonce, 16, 16, 100 + round * 3 + 6,
                          (key + round) % 7 != 0, (key + round) % 11 == 0);
        }
      }
      if ((key + round) % 8 == 0) {
        facts[key].nonce += 16;
        facts[key].balance = 1 + ((key * 31 + round * 17) % 1000);
      } else if ((key + round) % 13 == 0) {
        facts[key].balance += 7;  // incoming destination-only changes
      }
    }
    // Endpoint jumps exercise coalesced roots and changing expiry horizons.
    Access::compare(journal, reference, td::make_ref<JournalShardState>(1 + round * 3, facts), sources);
  }
  ASSERT_TRUE(Access::stats(journal).reused_observations > 5000u);
  ASSERT_TRUE(Access::stats(journal).unchanged_apply_skips > 0u);
  ASSERT_EQ(Access::stats(journal).failed_shards, 0u);
  ASSERT_EQ(Access::stats(journal).retained_sources, 256u);
}

}  // namespace ton::validator
