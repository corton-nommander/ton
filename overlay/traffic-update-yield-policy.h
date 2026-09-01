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

#include <cstdint>

namespace ton::overlay::detail {

class TrafficUpdateYieldPolicy {
 public:
  static constexpr std::uint32_t kUpdatesPerYield = 64;

  bool on_update() {
    if (++updates_since_yield_ < kUpdatesPerYield) {
      return false;
    }
    updates_since_yield_ = 0;
    return true;
  }

 private:
  std::uint32_t updates_since_yield_{0};
};

class FecCallbackYieldPolicy {
 public:
  static constexpr std::uint32_t kCallbacksPerYield = 64;

  bool on_callback() {
    if (++callbacks_since_yield_ < kCallbacksPerYield) {
      return false;
    }
    callbacks_since_yield_ = 0;
    return true;
  }

 private:
  std::uint32_t callbacks_since_yield_{0};
};

}  // namespace ton::overlay::detail
