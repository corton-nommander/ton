/*
    This file is part of TON Blockchain Library.
    It is distributed under the GNU Lesser General Public License, version 2
    or (at your option) any later version. See <http://www.gnu.org/licenses/>.
*/
#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "block/block-parse.h"
#include "block/transaction.h"
#include "td/utils/crypto.h"
#include "td/utils/logging.h"
#include "vm/cells.h"
#include "vm/dict.h"

namespace {
constexpr std::array<std::size_t, 7> update_counts{64, 80, 128, 192, 256, 384, 512};
constexpr std::array<unsigned, 4> worker_counts{1, 2, 4, 8};

struct Options {
  unsigned rounds = 7;
  unsigned iterations = 20;
  unsigned base_accounts = 4096;
};

unsigned parse_positive(const std::string& value) {
  std::size_t parsed = 0;
  const auto number = std::stoul(value, &parsed);
  if (parsed != value.size() || number == 0 || number > 1'000'000 || value.front() == '-') {
    throw std::invalid_argument("expected an integer between 1 and 1000000");
  }
  return static_cast<unsigned>(number);
}

td::Ref<vm::CellSlice> shard_account(td::uint64 balance, td::uint64 nonce) {
  // Use the same validated native Account builder as the collator. Fixture
  // construction and account-cell serialization are outside the trie timer.
  auto cells = block::build_native_account_state_cells_parallel({{balance, nonce, 0}}, 1);
  CHECK(cells.size() == 1 && cells.front().not_null());
  vm::CellBuilder builder;
  CHECK(builder.store_ref_bool(std::move(cells.front())) && builder.store_bits_bool(ton::Bits256{}) &&
        builder.store_ulong_rchk_bool(nonce, 64));
  return vm::load_cell_slice_ref(builder.finalize());
}

struct Fixture {
  std::vector<ton::StdSmcAddress> addresses;
  vm::AugmentedDictionary initial{256, block::tlb::aug_ShardAccounts};

  Fixture(std::size_t base_accounts, unsigned prefix_bits) {
    addresses.reserve(base_accounts + update_counts.back());
    for (std::size_t index = 0; index < base_accounts + update_counts.back(); ++index) {
      ton::StdSmcAddress address;
      td::sha256("staged-trie-benchmark/" + std::to_string(index), address.as_slice());
      if (prefix_bits == 2) {
        // Match the current fixed-depth payment-lane workload.
        address.as_slice()[0] = static_cast<char>((static_cast<unsigned char>(address.as_slice()[0]) & 0x3f) | 0x40);
      } else if (prefix_bits == 16) {
        // A deliberately clustered stress case, separate from lane realism.
        address.as_slice()[0] = 0x20;
        address.as_slice()[1] = 0;
      }
      addresses.push_back(address);
      if (index < base_accounts) {
        CHECK(initial.set(address, shard_account(1'000'000 + index, index)));
      }
    }
    CHECK(initial.validate_all());
  }
};

struct Workload {
  std::vector<vm::AugmentedDictionary::SetManyEntry> updates;
  td::Ref<vm::Cell> expected_root;

  Workload(const Fixture& fixture, std::size_t base_accounts, std::size_t update_count) {
    updates.reserve(update_count);
    for (std::size_t index = 0; index < update_count; ++index) {
      // 75% replacements spread throughout the old trie, 25% newly created
      // accounts. No address storage moves after SetManyEntry borrows a key.
      const auto address_index = index % 4 == 0 ? base_accounts + index : index * base_accounts / update_count;
      updates.emplace_back(fixture.addresses[address_index].cbits(), shard_account(2'000'000 + index, 10'000 + index));
    }
    std::sort(updates.begin(), updates.end(), [](const auto& left, const auto& right) {
      return td::bitstring::bits_memcmp(left.first, right.first, 256) < 0;
    });
    vm::AugmentedDictionary sequential{fixture.initial};
    for (const auto& [key, value] : updates) {
      CHECK(sequential.set(key, 256, value));
    }
    CHECK(sequential.validate_all());
    expected_root = sequential.get_root_cell();
  }
};

struct Timing {
  double wall_us = 0;
  double cpu_us = 0;
};

Timing run_once(const Fixture& fixture, Workload& workload, unsigned workers, bool tracked, bool validate) {
  // A fresh usage tree per operation models production's tracked old state
  // without carrying warmed proof-accounting state into the next operation.
  auto usage = tracked ? std::make_shared<vm::CellUsageTree>() : nullptr;
  auto root = fixture.initial.get_root_cell();
  if (usage) {
    root = vm::UsageCell::create(std::move(root), usage->root_ptr());
  }
  vm::AugmentedDictionary staged{std::move(root), 256, block::tlb::aug_ShardAccounts, false};
  const auto cpu_start = std::clock();
  const auto start = std::chrono::steady_clock::now();
  const bool applied = staged.set_many_sorted_parallel(td::as_span(workload.updates), workers);
  const auto stop = std::chrono::steady_clock::now();
  const auto cpu_stop = std::clock();
  CHECK(applied);
  CHECK(staged.get_root_cell()->get_hash() == workload.expected_root->get_hash());
  if (validate) {
    CHECK(staged.validate_all());
  }
  CHECK(cpu_start != std::clock_t(-1) && cpu_stop != std::clock_t(-1));
  return {std::chrono::duration<double, std::micro>(stop - start).count(),
          1'000'000.0 * static_cast<double>(cpu_stop - cpu_start) / CLOCKS_PER_SEC};
}
}  // namespace

int main(int argc, char** argv) {
  Options options;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "bench-staged-trie [--rounds 7] [--iterations 20] [--base-accounts 4096]\n"
                     "CSV samples cover 64/80/128/192/256/384/512 updates, 1/2/4/8 workers,\n"
                     "uniform/prefix2/prefix16 keys, and plain/tracked prior state. No policy is changed.\n";
        return 0;
      }
      if (option != "--rounds" && option != "--iterations" && option != "--base-accounts") {
        throw std::invalid_argument("unknown option: " + option);
      }
      if (++i == argc) {
        throw std::invalid_argument("missing value for " + option);
      }
      const auto value = parse_positive(argv[i]);
      if (option == "--rounds") {
        options.rounds = value;
      } else if (option == "--iterations") {
        options.iterations = value;
      } else {
        options.base_accounts = value;
      }
    }
    if (options.base_accounts < update_counts.back()) {
      throw std::invalid_argument("--base-accounts must be at least 512");
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }

  std::cout << "base_accounts,distribution,prior_state,updates,workers,round,iterations,wall_us_per_op,cpu_us_per_op\n"
            << std::fixed << std::setprecision(3);
  for (const unsigned prefix_bits : {0u, 2u, 16u}) {
    const Fixture fixture{options.base_accounts, prefix_bits};
    const char* distribution = prefix_bits == 0 ? "uniform" : prefix_bits == 2 ? "prefix2" : "prefix16";
    for (const bool tracked : {false, true}) {
      for (const auto update_count : update_counts) {
        Workload workload{fixture, options.base_accounts, update_count};
        for (const auto workers : worker_counts) {
          run_once(fixture, workload, workers, tracked, true);
          run_once(fixture, workload, workers, tracked, false);
        }
        for (unsigned round = 0; round < options.rounds; ++round) {
          // Rotate worker order between paired rounds to limit systematic
          // thermal/frequency bias; keep each case's samples together.
          for (std::size_t tier = 0; tier < worker_counts.size(); ++tier) {
            const auto workers = worker_counts[(tier + round) % worker_counts.size()];
            Timing total;
            for (unsigned iteration = 0; iteration < options.iterations; ++iteration) {
              const auto timing = run_once(fixture, workload, workers, tracked, false);
              total.wall_us += timing.wall_us;
              total.cpu_us += timing.cpu_us;
            }
            std::cout << options.base_accounts << ',' << distribution << ','
                      << (tracked ? "tracked" : "plain") << ',' << update_count << ',' << workers << ',' << round << ','
                      << options.iterations << ',' << total.wall_us / options.iterations << ','
                      << total.cpu_us / options.iterations << '\n';
          }
          std::cout.flush();
        }
      }
    }
  }
  return 0;
}
