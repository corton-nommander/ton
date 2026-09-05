/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>

#include "validator/consensus/simplex/state-resolver-policy.h"

namespace {

using namespace ton::validator::consensus::simplex;

struct TestBranch {
  std::optional<int> parent_id;
  std::set<int> own_nonce_floors;
};

struct TestBoundedRecord {
  ton::BlockSeqno seqno = 0;
  bool is_full = false;
  bool protected_exact_top = false;
};

void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

std::set<int> materialize(const std::map<int, TestBranch>& branches, int candidate, std::set<int> canonical) {
  return materialize_native_branch_floor_chain(
      branches, candidate, std::move(canonical),
      [](auto& target, const auto& added) { target.insert(added.begin(), added.end()); });
}

void test_record_actions() {
  require(select_native_branch_record_action(false, 7, 0, false) == NativeBranchRecordAction::retain);
  require(select_native_branch_record_action(true, 8, 7, false) == NativeBranchRecordAction::retain);

  // This is both the ordinary exact winner and notification-before-accept:
  // the already-known canonical root is promoted when its accept completes.
  require(select_native_branch_record_action(true, 7, 7, true) ==
          NativeBranchRecordAction::promote_and_discard);
  require(select_native_branch_record_action(true, 7, 7, false) == NativeBranchRecordAction::discard);
  require(select_native_branch_record_action(true, 6, 7, false) == NativeBranchRecordAction::discard);
}

void test_exact_ancestry_and_fallback() {
  std::map<int, TestBranch> branches{
      {10, {.parent_id = std::nullopt, .own_nonce_floors = {10}}},
      // Candidate 12 models an empty candidate: it contributes no nonce
      // delta but preserves the exact CandidateId edge to its full parent.
      {12, {.parent_id = 10, .own_nonce_floors = {}}},
      {11, {.parent_id = 12, .own_nonce_floors = {11}}},
      {20, {.parent_id = std::nullopt, .own_nonce_floors = {20}}},
      {21, {.parent_id = 20, .own_nonce_floors = {21}}},
  };

  auto left = materialize(branches, 11, {1});
  auto right = materialize(branches, 21, {1});
  require(left == std::set<int>{1, 10, 11});
  require(right == std::set<int>{1, 20, 21});
  require(!left.contains(20) && !left.contains(21));
  require(!right.contains(10) && !right.contains(11));

  // Pruning or restart can remove the exact parent. The child keeps its own
  // delta and falls back only to canonical progress, never to its sibling.
  branches.erase(10);
  auto pruned = materialize(branches, 11, {1});
  require(pruned == std::set<int>{1, 11});
  require(materialize(branches, 99, {1}) == std::set<int>{1});
}

void test_competing_branches_keep_distinct_same_source_floors() {
  using Floors = std::map<int, int>;
  struct Branch {
    std::optional<int> parent_id;
    Floors own_nonce_floors;
  };
  const Floors canonical{{1, 4}, {2, 7}};
  const std::map<int, Branch> branches{
      {10, {.parent_id = std::nullopt, .own_nonce_floors = {{1, 5}}}},
      {11, {.parent_id = 10, .own_nonce_floors = {{1, 6}}}},
      {20, {.parent_id = std::nullopt, .own_nonce_floors = {{1, 99}, {2, 80}}}},
  };
  auto merge = [](auto& target, const auto& added) {
    for (const auto& [source, nonce] : added) {
      target[source] = std::max(target[source], nonce);
    }
  };
  auto left = materialize_native_branch_floor_chain(branches, 11, canonical, merge);
  auto right = materialize_native_branch_floor_chain(branches, 20, canonical, merge);
  require(left == Floors{{1, 6}, {2, 7}});
  require(right == Floors{{1, 99}, {2, 80}});
  require(canonical == Floors{{1, 4}, {2, 7}});
}

void test_pruning_and_eviction_policy() {
  require(should_prune_native_branch_record(6, 7));
  require(should_prune_native_branch_record(7, 7));
  require(!should_prune_native_branch_record(8, 7));

  require(native_branch_eviction_precedes(6, true, 7, false));
  require(native_branch_eviction_precedes(7, false, 7, true));
  require(!native_branch_eviction_precedes(7, true, 7, false));
  static_assert(native_finalized_branch_record_limit == 32);

  std::map<int, TestBoundedRecord> records;
  for (int id = 0; id < 33; ++id) {
    records.emplace(id, TestBoundedRecord{.seqno = static_cast<ton::BlockSeqno>(id + 1), .is_full = true});
  }
  // Protect the otherwise-oldest exact pending top and add an empty alias at
  // the same next-oldest height. The shared production policy must evict that
  // alias, enforce the hard cap, and retain the protected exact record.
  records.at(0).protected_exact_top = true;
  records.at(1).is_full = false;
  int evicted_id = -1;
  auto evicted = bound_native_branch_records(
      records, native_finalized_branch_record_limit,
      [](const auto& record) { return record.second.protected_exact_top; },
      [](const auto& record) { return record.second.seqno; },
      [](const auto& record) { return record.second.is_full; },
      [&](const auto& record) { evicted_id = record.first; });
  require(evicted == 1);
  require(records.size() == native_finalized_branch_record_limit);
  require(records.contains(0));
  require(evicted_id == 1);
}

}  // namespace

int main() {
  test_record_actions();
  test_exact_ancestry_and_fallback();
  test_competing_branches_keep_distinct_same_source_floors();
  test_pruning_and_eviction_policy();
  return 0;
}
