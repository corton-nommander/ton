#pragma once
#include "vm/boc.h"
#include "vm/cells.h"
#include "vm/cellslice.h"

namespace proof_benchmark {
// Frozen pre-candidate CellSlice traversal: independent differential oracle.
// Keep it unchanged when optimizing production NewCellStorageStat.
class ReferenceCellStorageStat {
 public:
  using Stat = vm::NewCellStorageStat::Stat;
  Stat get_stat() const { return stat_; }
  Stat get_proof_stat() const { return proof_stat_; }
  Stat get_total_stat() const { return stat_ + proof_stat_; }
  void add_cell(td::Ref<vm::Cell> cell) { dfs(std::move(cell), true, false); }
  void add_proof(td::Ref<vm::Cell> cell, const vm::CellUsageTree* tree) {
    CHECK(tree);
    usage_tree_ = tree;
    dfs(std::move(cell), false, true);
  }
  void add_cell_and_proof(td::Ref<vm::Cell> cell, const vm::CellUsageTree* tree) {
    CHECK(tree);
    usage_tree_ = tree;
    dfs(std::move(cell), true, true);
  }
  Stat tentative_add_cell(td::Ref<vm::Cell> cell) const {
    ReferenceCellStorageStat trial;
    trial.parent_ = this;
    trial.add_cell(std::move(cell));
    return trial.get_stat();
  }
  Stat tentative_add_proof(td::Ref<vm::Cell> cell, const vm::CellUsageTree* tree) const {
    ReferenceCellStorageStat trial;
    trial.parent_ = this;
    trial.add_proof(std::move(cell), tree);
    return trial.get_proof_stat();
  }
 private:
  using Cell = vm::Cell;
  template <class T> using Ref = td::Ref<T>;
  const vm::CellUsageTree* usage_tree_{nullptr};
  td::HashSet<vm::Cell::Hash> seen_;
  Stat stat_;
  td::HashSet<vm::Cell::Hash> proof_seen_;
  Stat proof_stat_;
  const ReferenceCellStorageStat* parent_{nullptr};
  void dfs(Ref<Cell> cell, bool need_stat, bool need_proof_stat);
};

void ReferenceCellStorageStat::dfs(Ref<Cell> cell, bool need_stat, bool need_proof_stat) {
  if (cell.is_null()) {
    // FIXME: save error flag?
    return;
  }
  if (need_stat) {
    stat_.internal_refs++;
    if ((parent_ && parent_->seen_.count(cell->get_hash()) != 0) || !seen_.insert(cell->get_hash()).second) {
      need_stat = false;
    } else {
      stat_.cells++;
    }
  }

  if (need_proof_stat) {
    auto tree_node = cell->get_tree_node();
    if (!tree_node.empty() && tree_node.is_from_tree(usage_tree_)) {
      proof_stat_.external_refs++;
      need_proof_stat = false;
    } else {
      proof_stat_.internal_refs++;
      if ((parent_ && parent_->proof_seen_.count(cell->get_hash()) != 0) ||
          !proof_seen_.insert(cell->get_hash()).second) {
        need_proof_stat = false;
      } else {
        proof_stat_.cells++;
      }
    }
  }

  if (!need_proof_stat && !need_stat) {
    return;
  }
  vm::CellSlice cs{vm::NoVm{}, std::move(cell)};
  if (need_stat) {
    stat_.bits += cs.size();
  }
  if (need_proof_stat) {
    proof_stat_.bits += cs.size();
  }
  while (cs.size_refs()) {
    dfs(cs.fetch_ref(), need_stat, need_proof_stat);
  }
}

}  // namespace proof_benchmark
