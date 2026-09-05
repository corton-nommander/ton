/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

#include "ton/ton-types.h"

namespace ton::validator::detail {

// Fixed-capacity scratch storage for one native signed work. Addresses remain
// sorted to preserve the iteration order of the std::set/std::map containers
// this replaces. A snapshot is captured at most once, before the first write.
template <class Snapshot, std::size_t MaxAddresses>
class NativeWorkScratch {
  static_assert(MaxAddresses > 0);

 public:
  static constexpr std::size_t capacity() noexcept {
    return MaxAddresses;
  }

  std::size_t size() const noexcept {
    return size_;
  }

  bool empty() const noexcept {
    return size_ == 0;
  }

  bool contains(const StdSmcAddress& address) const {
    auto it = find(address);
    return it != active_end() && it->address == address;
  }

  // Returns false only if inserting a new address would exceed the bound.
  // Existing addresses remain admissible even when the scratch is full.
  bool add_address(const StdSmcAddress& address) {
    auto it = find(address);
    if (it != active_end() && it->address == address) {
      return true;
    }
    if (size_ == MaxAddresses) {
      return false;
    }
    std::move_backward(it, active_end(), active_end() + 1);
    *it = Slot{address, std::nullopt};
    ++size_;
    return true;
  }

  // All work endpoints must be declared before snapshot capture or state
  // mutation; source-only preflight may read state first. Repeated calls keep
  // the earliest snapshot so rollback restores pre-work state even when
  // several transfers touch the same account.
  bool capture_before(const StdSmcAddress& address, Snapshot snapshot) {
    auto it = find(address);
    if (it == active_end() || it->address != address) {
      return false;
    }
    if (!it->snapshot) {
      it->snapshot.emplace(std::move(snapshot));
    }
    return true;
  }

  template <class Function>
  void for_each_address(Function&& function) const {
    for (auto it = slots_.begin(); it != active_end(); ++it) {
      function(it->address);
    }
  }

  template <class Function>
  void for_each_snapshot(Function&& function) const {
    for (auto it = slots_.begin(); it != active_end(); ++it) {
      if (it->snapshot) {
        function(it->address, *it->snapshot);
      }
    }
  }

  void clear() {
    for (auto it = slots_.begin(); it != active_end(); ++it) {
      it->snapshot.reset();
    }
    size_ = 0;
  }

 private:
  struct Slot {
    StdSmcAddress address;
    std::optional<Snapshot> snapshot;
  };

  using Iterator = typename std::array<Slot, MaxAddresses>::iterator;
  using ConstIterator = typename std::array<Slot, MaxAddresses>::const_iterator;

  Iterator active_end() {
    return slots_.begin() + size_;
  }

  ConstIterator active_end() const {
    return slots_.begin() + size_;
  }

  Iterator find(const StdSmcAddress& address) {
    return std::lower_bound(slots_.begin(), active_end(), address,
                            [](const Slot& slot, const StdSmcAddress& key) { return slot.address < key; });
  }

  ConstIterator find(const StdSmcAddress& address) const {
    return std::lower_bound(slots_.begin(), active_end(), address,
                            [](const Slot& slot, const StdSmcAddress& key) { return slot.address < key; });
  }

  std::array<Slot, MaxAddresses> slots_{};
  std::size_t size_{0};
};

}  // namespace ton::validator::detail
