/*
    This file is part of TON Blockchain Library.
    Licensed under the GNU Lesser General Public License, version 2 or later.
*/
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>

#include "td/utils/Time.h"
#include "td/utils/check.h"
#include "ton/ton-shard.h"
#include "ton/ton-types.h"

namespace ton::validator {

// Process-local coordination for four fixed native admission owners and their
// generic-message coordinator. Account facts, reservations and branch cursors
// remain owner-local. Never hold either a read fence or a quota lock over await.
class NativePoolOwnerCoordination {
 public:
  explicit NativePoolOwnerCoordination(unsigned owners, std::size_t max_mempool)
      : owners(owners), max_mempool_(max_mempool) {
    CHECK(owners == 4);
  }

  static constexpr std::size_t PER_ADDRESS_LIMIT = 8192;
  static constexpr double CHECKED_MESSAGE_WINDOW_SECONDS = 10.0;
  const unsigned owners;
  const unsigned prefix_bits{2};
  unsigned total_signature_workers{0};
  std::shared_mutex state_fence;
  std::atomic<td::uint64> published_generation{0};
  std::atomic<bool> topology_ready{false};

  // An inline promise continuation may enter another admission on this thread.
  // shared_mutex has no recursive reader guarantee, especially with a waiting
  // writer. A nested synchronous scope borrows its outer scope's lock. A state
  // publisher must defer rather than attempt an upgrade when held_on_this_thread
  // is true. These scopes must never survive coroutine suspension.
  class ReadFence {
   public:
    explicit ReadFence(NativePoolOwnerCoordination *coordination) : coordination_(coordination) {
      CHECK(coordination_ != nullptr);
      if (!held_on_this_thread(coordination_)) {
        lock_.emplace(coordination_->state_fence);
      }
      previous_ = active_;
      active_ = this;
    }
    ~ReadFence() {
      CHECK(active_ == this);
      active_ = previous_;
    }
    ReadFence(const ReadFence &) = delete;
    ReadFence &operator=(const ReadFence &) = delete;

    static bool held_on_this_thread(const NativePoolOwnerCoordination *coordination) {
      for (auto *scope = active_; scope != nullptr; scope = scope->previous_) {
        if (scope->coordination_ == coordination) {
          return true;
        }
      }
      return false;
    }

   private:
    NativePoolOwnerCoordination *coordination_;
    ReadFence *previous_{nullptr};
    std::optional<std::shared_lock<std::shared_mutex>> lock_;
    inline static thread_local ReadFence *active_{nullptr};
  };

  unsigned owner_for(WorkchainId workchain, const StdSmcAddress &address) const {
    return workchain == basechainId
               ? static_cast<unsigned>(extract_addr_prefix(workchain, address).account_id_prefix >> 62)
               : 0;
  }
  unsigned owner_for(ShardIdFull shard) const {
    return shard.workchain == basechainId ? static_cast<unsigned>(shard.shard >> 62) : 0;
  }
  bool contains_leaf(ShardIdFull shard) const {
    return shard.workchain != basechainId || shard_pfx_len(shard.shard) >= static_cast<int>(prefix_bits);
  }

  // A permit owns one actual index membership, independent of a retained BOC
  // or producer view. Erasure explicitly resets it; destruction is the failure
  // and shutdown fallback. Promotion first acquires its target-priority permit
  // and only then erases its previous membership.
  class Permit {
   public:
    ~Permit() { owner_->release(priority_, address_); }
    Permit(const Permit &) = delete;
    Permit &operator=(const Permit &) = delete;

   private:
    friend class NativePoolOwnerCoordination;
    Permit(std::shared_ptr<NativePoolOwnerCoordination> owner, int priority,
           std::pair<WorkchainId, StdSmcAddress> address)
        : owner_(std::move(owner)), priority_(priority), address_(std::move(address)) {
    }
    std::shared_ptr<NativePoolOwnerCoordination> owner_;
    int priority_;
    std::pair<WorkchainId, StdSmcAddress> address_;
  };

  std::unique_ptr<Permit> try_acquire(const std::shared_ptr<NativePoolOwnerCoordination> &self, int priority,
                                     WorkchainId workchain, const StdSmcAddress &address) {
    CHECK(self.get() == this);
    std::lock_guard lock(quota_mutex_);
    auto it = quotas_.find(priority);
    if ((it == quotas_.end() ? 0 : it->second.count) >= max_mempool_) {
      return {};
    }
    auto &quota = quotas_[priority];
    auto key = std::make_pair(workchain, address);
    auto found = quota.address_counts.find(key);
    if (found != quota.address_counts.end() && found->second >= PER_ADDRESS_LIMIT) {
      return {};
    }
    auto &address_count = quota.address_counts[key];
    auto permit = std::unique_ptr<Permit>(new Permit(self, priority, key));
    ++address_count;
    ++quota.count;
    return permit;
  }

  std::size_t count(int priority) const {
    std::lock_guard lock(quota_mutex_);
    auto it = quotas_.find(priority);
    return it == quotas_.end() ? 0 : it->second.count;
  }
  std::size_t count() const {
    std::lock_guard lock(quota_mutex_);
    std::size_t total = 0;
    for (const auto &[_, quota] : quotas_) {
      total += quota.count;
    }
    return total;
  }
  std::size_t address_count(int priority, WorkchainId workchain, const StdSmcAddress &address) const {
    std::lock_guard lock(quota_mutex_);
    auto it = quotas_.find(priority);
    if (it == quotas_.end()) {
      return 0;
    }
    auto found = it->second.address_counts.find({workchain, address});
    return found == it->second.address_counts.end() ? 0 : found->second;
  }
  std::size_t limit() const {
    std::lock_guard lock(quota_mutex_);
    return max_mempool_;
  }
  void set_limit(std::size_t max_mempool) {
    std::lock_guard lock(quota_mutex_);
    // Reductions preserve existing memberships and prevent new admissions
    // until enough entries have been erased, just as in the single pool.
    max_mempool_ = max_mempool;
  }

  // Keep the original approximate 10-second checked-message window: current
  // plus previous five-second halves. This is shared by generic and native
  // paths, separate from membership quotas, and independent of priority.
  std::size_t get_msg_count(WorkchainId workchain, const StdSmcAddress &address) {
    return checked_message_count(workchain, address, false, td::Timestamp::now());
  }
  std::size_t inc_msg_count(WorkchainId workchain, const StdSmcAddress &address) {
    return checked_message_count(workchain, address, true, td::Timestamp::now());
  }

 private:
  friend class NativePreparedOwnerCoordinationTestAccess;
  using Address = std::pair<WorkchainId, StdSmcAddress>;
  struct Quota {
    std::size_t count{0};
    std::map<Address, std::size_t> address_counts;
  };
  void release(int priority, const Address &address) {
    std::lock_guard lock(quota_mutex_);
    auto it = quotas_.find(priority);
    CHECK(it != quotas_.end() && it->second.count != 0);
    auto found = it->second.address_counts.find(address);
    CHECK(found != it->second.address_counts.end() && found->second != 0);
    if (--found->second == 0) {
      it->second.address_counts.erase(found);
    }
    if (--it->second.count == 0) {
      CHECK(it->second.address_counts.empty());
      quotas_.erase(it);
    }
  }
  std::size_t checked_message_count(WorkchainId workchain, const StdSmcAddress &address, bool increment,
                                    td::Timestamp now) {
    std::lock_guard lock(checked_counter_mutex_);
    while (cleanup_at_.is_in_past(now)) {
      counter_prev_ = std::move(counter_cur_);
      counter_cur_.clear();
      if (counter_prev_.empty()) {
        cleanup_at_ = td::Timestamp::in(CHECKED_MESSAGE_WINDOW_SECONDS / 2.0, now);
        break;
      }
      cleanup_at_ += CHECKED_MESSAGE_WINDOW_SECONDS / 2.0;
    }
    Address key{workchain, address};
    auto prev = counter_prev_.find(key);
    auto count = prev == counter_prev_.end() ? 0 : prev->second;
    if (increment) {
      return count + ++counter_cur_[key];
    }
    auto cur = counter_cur_.find(key);
    return count + (cur == counter_cur_.end() ? 0 : cur->second);
  }

  mutable std::mutex quota_mutex_;
  std::map<int, Quota> quotas_;
  std::size_t max_mempool_;
  std::mutex checked_counter_mutex_;
  std::map<Address, std::size_t> counter_cur_, counter_prev_;
  td::Timestamp cleanup_at_ = td::Timestamp::now();
};

}  // namespace ton::validator
