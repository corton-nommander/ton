/*
    This file is part of TON Blockchain Library.
    TON Blockchain Library is free software under the GNU Lesser General
    Public License, version 2 or (at your option) any later version.
    See LICENSE.LGPL in the source distribution for details.
*/
#include "native-canonical-journal.h"

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/mc-config.h"
#include "ext-message-pool.hpp"
#include "tl/tlblib.hpp"
#include "vm/dict.h"

namespace ton::validator {
namespace {

struct ChangedAccount {
  StdSmcAddress address;
  td::Ref<vm::CellSlice> value;
};

bool can_diff(const NativeCanonicalJournalEndpoint &before, const NativeCanonicalJournalEndpoint &after,
              const BlockIdExt &successful_top) {
  // Both endpoints came from the shard-client-confirmed MC update path. A
  // coalesced endpoint diff can span several shard blocks, but cannot cross
  // a topology change, conflicting identity, backwards time or missing scan.
  return before.shard_block == successful_top && before.accounts.not_null() &&
         before.shard_block.shard_full() == after.shard_block.shard_full() &&
         before.masterchain_block.seqno() <= after.masterchain_block.seqno() &&
         (before.masterchain_block.seqno() != after.masterchain_block.seqno() ||
          (before.masterchain_block == after.masterchain_block && before.masterchain_root == after.masterchain_root)) &&
         before.shard_block.seqno() <= after.shard_block.seqno() &&
         (before.shard_block.seqno() != after.shard_block.seqno() ||
          (before.shard_block == after.shard_block && before.state_root == after.state_root)) &&
         before.gen_lt <= after.gen_lt && before.gen_utime <= after.gen_utime;
}

}  // namespace

void ExtMessagePool::prune_native_journal_endpoints(const NativeShardTopFingerprint &topology) {
  for (auto it = native_canonical_journal_endpoints_.begin(); it != native_canonical_journal_endpoints_.end();) {
    if (topology.find(it->first) == topology.end()) {
      native_canonical_journal_telemetry_.retained_sources -= it->second.observations.size();
      ++native_canonical_journal_telemetry_.topology_evictions;
      it = native_canonical_journal_endpoints_.erase(it);
    } else {
      ++it;
    }
  }
}

bool ExtMessagePool::advance_unchanged_native_journal_account(
    const NativeAddress &address, const NativeCanonicalAccountObservation &observation, UnixTime utime, LogicalTime lt) {
  auto watermark_it = native_nonce_watermarks_.find(address);
  if (watermark_it == native_nonce_watermarks_.end()) {
    return false;
  }
  auto &watermark = watermark_it->second;
  if (!watermark.observed_balance || watermark.observed_next_nonce != observation.nonce ||
      watermark.observed_balance.value() != observation.balance || lt < watermark.observed_lt) {
    return false;
  }
  auto account_it = native_accounts_.find(address);
  if (account_it != native_accounts_.end()) {
    auto &account = account_it->second;
    if (account.observed_nonce > observation.nonce) {
      return false;
    }
    td::uint64 prefix_amount = 0;
    // Retain a cheap reservation walk rather than introduce a second expiry
    // index with a new mutation lifecycle. Every case with effects uses the
    // original canonical application path, including expiry at an unchanged
    // accounts root and stale/incomplete verification revisions.
    for (const auto &[nonce, work] : account.messages) {
      ++native_canonical_journal_telemetry_.reservation_checks;
      auto amount = work.required_amount();
      if (!work.has_valid_interval(nonce) || nonce < observation.nonce ||
          work.valid_until <= std::max(utime, account.observed_utime) ||
          work.account_revision != watermark.revision || !amount || prefix_amount > observation.balance ||
          amount.value() > observation.balance - prefix_amount) {
        return false;
      }
      prefix_amount += amount.value();
    }
    account.observed_nonce = std::max(account.observed_nonce, observation.nonce);
    account.observed_utime = std::max(account.observed_utime, utime);
  }
  // Advancing the exact state's LT still fences older asynchronous admission
  // observations. Do not fabricate a revision: the account facts are unchanged.
  watermark.observed_lt = lt;
  auto tracked = locally_accepted_native_nonces_.find(address);
  if (tracked != locally_accepted_native_nonces_.end() && observation.nonce > tracked->second) {
    locally_accepted_native_nonces_.erase(tracked);
  }
  prune_native_reconciliation_target_if_idle(address);
  return true;
}

td::Status ExtMessagePool::reconcile_native_journal_shard(
    const MasterchainState &masterchain, const td::Ref<ShardState> &shard_state,
    const std::vector<NativeAddress> &sources, std::set<NativeAddress> &changed_sources) {
  auto &j = native_canonical_journal_telemetry_;
  ++j.attempts;
  if (shard_state.is_null() || masterchain.root_cell().is_null() ||
      !masterchain.get_block_id().is_masterchain()) {
    ++j.failed_shards;
    return td::Status::Error("native journal requires an exact canonical masterchain/shard state");
  }
  const auto shard_block = shard_state->get_block_id();
  for (const auto &source : sources) {
    if (!shard_contains(shard_block.shard_full(), extract_addr_prefix(source.first, source.second))) {
      ++j.failed_shards;
      return td::Status::Error("native journal source is outside its exact shard state");
    }
  }
  auto reference = masterchain.get_shard_from_config(shard_block.shard_full(), true);
  if (reference.is_null() || reference->top_block_id() != shard_block) {
    ++j.failed_shards;
    return td::Status::Error("native journal shard is not the exact canonical masterchain reference");
  }
  block::gen::ShardStateUnsplit::Record info;
  auto root = shard_state->root_cell();
  if (root.is_null() || !tlb::unpack_cell(root, info)) {
    ++j.failed_shards;
    return td::Status::Error("cannot unpack applied shard state for native journal");
  }
  block::ShardId header_shard{info.shard_id};
  if (!header_shard.is_valid() || ShardIdFull(header_shard) != shard_block.shard_full() ||
      info.seq_no != shard_block.seqno() || info.accounts.is_null() ||
      shard_state->root_hash() != RootHash{root->get_hash().bits()}) {
    ++j.failed_shards;
    return td::Status::Error("native journal shard header/root does not match its exact state identity");
  }
  NativeCanonicalJournalEndpoint next{.masterchain_block = masterchain.get_block_id(),
                                      .masterchain_root = RootHash{masterchain.root_cell()->get_hash().bits()},
                                      .shard_block = shard_block,
                                      .state_root = RootHash{root->get_hash().bits()},
                                      .accounts = info.accounts,
                                      .gen_utime = info.gen_utime,
                                      .gen_lt = info.gen_lt,
                                      .observations = {}};
  auto prior = native_canonical_journal_endpoints_.find(shard_block.shard_full());
  auto successful = native_reconciliation_successful_shard_tops_.find(shard_block.shard_full());
  const auto replaced_sources = prior == native_canonical_journal_endpoints_.end() ? 0 : prior->second.observations.size();
  const bool within_budget = sources.size() <= MAX_NATIVE_JOURNAL_SOURCES &&
      j.retained_sources - replaced_sources <= MAX_NATIVE_JOURNAL_SOURCES - sources.size() &&
      (prior != native_canonical_journal_endpoints_.end() ||
       native_canonical_journal_endpoints_.size() < MAX_NATIVE_JOURNAL_SHARDS);
  bool use_diff = within_budget && prior != native_canonical_journal_endpoints_.end() &&
      successful != native_reconciliation_successful_shard_tops_.end() &&
      can_diff(prior->second, next, successful->second);
  if (!within_budget) {
    ++j.budget_fallbacks;
  } else if (prior == native_canonical_journal_endpoints_.end()) {
    ++j.bootstrap;
  } else if (!use_diff) {
    ++j.continuity_fallbacks;
  }

  vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(info.accounts), 256, block::tlb::aug_ShardAccounts};
  std::vector<ChangedAccount> changes;
  if (use_diff) {
    const double started = native_reconciliation_profile_enabled_ ? td::Time::now() : 0;
    bool exceeded_budget = false;
    try {
      vm::AugmentedDictionary old_accounts{vm::load_cell_slice_ref(prior->second.accounts), 256,
                                          block::tlb::aug_ShardAccounts};
      use_diff = old_accounts.scan_diff(accounts, [&](td::ConstBitPtr key, int key_len,
                                                      td::Ref<vm::CellSlice>, td::Ref<vm::CellSlice> new_value) {
        if (key_len != 256) {
          return false;
        }
        if (changes.size() >= MAX_NATIVE_JOURNAL_CHANGED_ACCOUNTS) {
          exceeded_budget = true;
          return false;
        }
        changes.push_back({StdSmcAddress{key}, accounts.extract_value(std::move(new_value))});
        return true;
      }, 2);
    } catch (vm::VmError &) {
      use_diff = false;
    } catch (vm::VmVirtError &) {
      use_diff = false;
    }
    if (native_reconciliation_profile_enabled_) {
      j.diff.observe(td::Time::now() - started);
    }
    if (!use_diff) {
      ++j.diff_fallbacks;
      j.budget_fallbacks += exceeded_budget;
      changes.clear();
    } else {
      j.diff_changed_accounts += changes.size();
      std::sort(changes.begin(), changes.end(), [](const auto &a, const auto &b) { return a.address < b.address; });
    }
  }
  if (within_budget) {
    next.observations.reserve(sources.size());
  }
  td::Status first_error;
  for (const auto &address : sources) {
    NativeCanonicalAccountObservation observation{address.second, 0, 0};
    bool reused = false;
    td::Ref<vm::CellSlice> value;
    bool have_value = false;
    if (use_diff) {
      auto change = std::lower_bound(changes.begin(), changes.end(), address.second,
                                    [](const auto &entry, const auto &key) { return entry.address < key; });
      if (change != changes.end() && change->address == address.second) {
        value = change->value;
        have_value = true;  // null is a deletion, never an unchanged source
        ++j.diff_tracked_changed;
      } else {
        const auto &observations = prior->second.observations;
        auto old = std::lower_bound(observations.begin(), observations.end(), address.second,
                                   [](const auto &entry, const auto &key) { return entry.address < key; });
        if (old != observations.end() && old->address == address.second) {
          observation = *old;
          reused = true;
          ++j.reused_observations;
        }
      }
    }
    if (!reused) {
      if (!have_value) {
        ++native_reconciliation_account_lookups_;
        ++native_reconciliation_telemetry_.account_lookups;
        ++j.lookup_bootstraps;
        const double started = native_reconciliation_profile_enabled_ ? td::Time::now() : 0;
        value = accounts.lookup(address.second);
        if (native_reconciliation_profile_enabled_) {
          native_reconciliation_telemetry_.lookup.observe(td::Time::now() - started);
        }
      }
      native_reconciliation_telemetry_.account_empty += value.is_null();
      block::Account account;
      const double started = native_reconciliation_profile_enabled_ ? td::Time::now() : 0;
      const bool unpacked = account.unpack(value, info.gen_utime, false);
      if (native_reconciliation_profile_enabled_) {
        native_reconciliation_telemetry_.unpack.observe(td::Time::now() - started);
      }
      if (!unpacked || account.status != block::Account::acc_uninit || !account.is_native) {
        if (!unpacked) {
          ++native_reconciliation_telemetry_.account_unpack_failures;
        } else {
          ++native_reconciliation_telemetry_.account_kind_failures;
        }
        if (first_error.is_ok()) {
          first_error = td::Status::Error("cannot decode a tracked native account at the canonical journal endpoint");
        }
        continue;
      }
      auto balance = account.native_balance_uint64();
      if (!balance) {
        ++native_reconciliation_telemetry_.account_balance_failures;
        if (first_error.is_ok()) {
          first_error = td::Status::Error("tracked native account balance is not uint64 grams");
        }
        continue;
      }
      observation.nonce = account.native_nonce;
      observation.balance = balance.value();
    }
    if (reused && advance_unchanged_native_journal_account(address, observation, info.gen_utime, info.gen_lt)) {
      ++j.unchanged_apply_skips;
    } else {
      ++j.apply_required;
      auto applied = apply_reconciled_native_account_state(address, observation.nonce, observation.balance,
                                                            info.gen_utime, info.gen_lt);
      if (applied.is_error()) {
        if (first_error.is_ok()) {
          first_error = applied.move_as_error_prefix("cannot apply canonical journal account state: ");
        }
        continue;
      }
      if (applied.ok()) {
        changed_sources.insert(address);
      }
    }
    if (within_budget) {
      next.observations.push_back(observation);
    }
  }
  if (first_error.is_error()) {
    // Some independent sources may already have progressed, as in the full
    // scan. Never publish a partial endpoint; repeating it remains idempotent.
    ++j.failed_shards;
    return first_error;
  }
  if (within_budget) {
    if (!std::is_sorted(next.observations.begin(), next.observations.end(),
                        [](const auto &a, const auto &b) { return a.address < b.address; })) {
      std::sort(next.observations.begin(), next.observations.end(),
                [](const auto &a, const auto &b) { return a.address < b.address; });
    }
    j.retained_sources = j.retained_sources - replaced_sources + next.observations.size();
    j.peak_retained_sources = std::max(j.peak_retained_sources, j.retained_sources);
    native_canonical_journal_endpoints_[shard_block.shard_full()] = std::move(next);
  } else if (prior != native_canonical_journal_endpoints_.end()) {
    j.retained_sources -= prior->second.observations.size();
    native_canonical_journal_endpoints_.erase(prior);
  }
  ++j.completed;
  return td::Status::OK();
}

}  // namespace ton::validator
