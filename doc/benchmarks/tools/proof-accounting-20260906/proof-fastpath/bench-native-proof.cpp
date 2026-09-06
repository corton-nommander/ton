/*
    TON Blockchain Library benchmark artifact; LGPL version 2 or later.
    No target is added to normal builds or CTest by this isolated driver.
*/
#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "block/block-parse.h"
#include "block/transaction.h"
#include "td/utils/crypto.h"
#include "td/utils/logging.h"
#include "vm/boc.h"
#include "vm/cells.h"
#include "vm/dict.h"
#include "reference-cell-storage-stat.h"

namespace {
constexpr std::array<unsigned, 9> populations{8, 16, 32, 64, 80, 128, 192, 256, 512};
constexpr std::array<unsigned, 3> checkpoint_counts{1, 4, 16};
constexpr std::array<const char*, 3> modes{"plain_prior", "tracked_prior", "foreign_usage"};
struct Options {
  unsigned rounds = 3;
  unsigned iterations = 3;
  unsigned base_accounts = 4096;
  unsigned base_proof_leaves = 64;
  std::string mode = "all";
};

unsigned parse_unsigned(const std::string& value, bool allow_zero = false) {
  std::size_t parsed = 0;
  auto number = std::stoul(value, &parsed);
  if (parsed != value.size() || (!allow_zero && number == 0) || number > 1'000'000 || value.front() == '-') {
    throw std::invalid_argument("invalid bounded unsigned integer: " + value);
  }
  return static_cast<unsigned>(number);
}

td::Ref<vm::CellSlice> shard_account(td::uint64 balance, td::uint64 nonce) {
  auto cells = block::build_native_account_state_cells_parallel({{balance, nonce, 0}}, 1);
  CHECK(cells.size() == 1 && cells.front().not_null());
  vm::CellBuilder builder;
  CHECK(builder.store_ref_bool(std::move(cells.front())) && builder.store_bits_bool(ton::Bits256{}) &&
        builder.store_ulong_rchk_bool(nonce, 64));
  return vm::load_cell_slice_ref(builder.finalize());
}

td::Ref<vm::Cell> base_proof(unsigned leaves) {
  std::vector<td::Ref<vm::Cell>> level;
  level.reserve(leaves);
  for (unsigned index = 0; index < leaves; ++index) {
    vm::CellBuilder builder;
    CHECK(builder.store_ulong_rchk_bool(0xface0000u + index, 32));
    level.push_back(builder.finalize());
  }
  while (level.size() > 1) {
    std::vector<td::Ref<vm::Cell>> next;
    next.reserve((level.size() + 3) / 4);
    for (std::size_t index = 0; index < level.size(); index += 4) {
      vm::CellBuilder builder;
      CHECK(builder.store_ulong_rchk_bool(0xface, 16));
      for (std::size_t child = index; child < std::min(level.size(), index + 4); ++child) {
        CHECK(builder.store_ref_bool(level[child]));
      }
      next.push_back(builder.finalize());
    }
    level = std::move(next);
  }
  return level.empty() ? td::Ref<vm::Cell>{} : level.front();
}

struct Fixture {
  unsigned base_accounts;
  std::vector<ton::StdSmcAddress> addresses;
  vm::AugmentedDictionary initial{256, block::tlb::aug_ShardAccounts};
  td::Ref<vm::Cell> pre_native_root;

  Fixture(const Options& options, unsigned prefix_bits) : base_accounts(options.base_accounts) {
    const std::size_t address_count = base_accounts + populations.back() * checkpoint_counts.back();
    addresses.reserve(address_count);
    for (std::size_t index = 0; index < address_count; ++index) {
      ton::StdSmcAddress address;
      td::sha256("native-proof-benchmark/" + std::to_string(index), address.as_slice());
      if (prefix_bits == 2) {
        address.as_slice()[0] = static_cast<char>((static_cast<unsigned char>(address.as_slice()[0]) & 0x3f) | 0x40);
      }
      addresses.push_back(address);
      if (index < base_accounts) {
        CHECK(initial.set(address, shard_account(1'000'000 + index, index)));
      }
    }
    CHECK(initial.validate_all());
    pre_native_root = base_proof(options.base_proof_leaves);
  }
};

struct Expected {
  vm::NewCellStorageStat::Stat ordinary;
  vm::NewCellStorageStat::Stat proof;
};

struct Workload {
  std::shared_ptr<vm::CellUsageTree> usage = std::make_shared<vm::CellUsageTree>();
  std::vector<td::Ref<vm::Cell>> roots;
  std::vector<Expected> expected;
  vm::NewCellStorageStat base;
  bool foreign;

  Workload(const Fixture& fixture, unsigned updates, const std::string& mode) : foreign(mode == "foreign_usage") {
    auto root = fixture.initial.get_root_cell();
    if (mode == "tracked_prior") {
      root = vm::UsageCell::create(std::move(root), usage->root_ptr());
    }
    vm::AugmentedDictionary staged{std::move(root), 256, block::tlb::aug_ShardAccounts, false};
    roots.reserve(checkpoint_counts.back());
    expected.reserve(checkpoint_counts.back());
    base.add_proof(fixture.pre_native_root, usage.get());
    proof_benchmark::ReferenceCellStorageStat reference_base;
    reference_base.add_proof(fixture.pre_native_root, usage.get());
    CHECK(base.get_total_stat() == reference_base.get_total_stat());
    for (unsigned checkpoint = 0; checkpoint < checkpoint_counts.back(); ++checkpoint) {
      // Exact checkpoints: 75% replacements and 25% inserts per update set.
      // Native values and dictionary building stay outside every proof timer.
      for (unsigned index = 0; index < updates; ++index) {
        const auto address_index = index % 4 == 0
            ? fixture.base_accounts + checkpoint * updates + index
            : (checkpoint * updates + index * fixture.base_accounts / updates) % fixture.base_accounts;
        CHECK(staged.set(fixture.addresses[address_index],
                         shard_account(2'000'000 + checkpoint * updates + index,
                                       10'000 + checkpoint * updates + index)));
      }
      CHECK(staged.validate_all());
      roots.push_back(staged.get_root_cell());
      auto reference_root = roots.back();
      auto foreign_tree = std::make_shared<vm::CellUsageTree>();
      if (foreign) {
        // A foreign UsageCell propagates wrappers to every descendant. This
        // separately reports the cost of failing the concrete DataCell guard.
        reference_root = vm::UsageCell::create(std::move(reference_root), foreign_tree->root_ptr());
      }
      auto reference_trial = reference_base;
      reference_trial.add_proof(std::move(reference_root), usage.get());
      expected.push_back({reference_trial.get_stat(), reference_trial.get_proof_stat()});
    }
  }
};

struct Timing {
  double wall_us = 0;
  double cpu_us = 0;
  void add(const Timing& other) { wall_us += other.wall_us; cpu_us += other.cpu_us; }
};

template <class F>
Timing time_action(F&& action) {
  const auto cpu_start = std::clock();
  const auto start = std::chrono::steady_clock::now();
  action();
  const auto end = std::chrono::steady_clock::now();
  const auto cpu_end = std::clock();
  CHECK(cpu_start != std::clock_t(-1) && cpu_end != std::clock_t(-1));
  return {std::chrono::duration<double, std::micro>(end - start).count(),
          1'000'000.0 * static_cast<double>(cpu_end - cpu_start) / CLOCKS_PER_SEC};
}

struct Sample {
  Timing copy;
  Timing proof;
};

Sample run_once(const Workload& workload, unsigned checkpoints) {
  Sample result;
  const auto base_ordinary = workload.base.get_stat();
  const auto base_proof_stat = workload.base.get_proof_stat();
  for (unsigned index = 0; index < checkpoints; ++index) {
    auto root = workload.roots[index];
    // Use a new foreign usage tree for every measured traversal. The oracle
    // and earlier rounds cannot warm its load callbacks or child node cache.
    auto foreign_tree = std::make_shared<vm::CellUsageTree>();
    if (workload.foreign) {
      root = vm::UsageCell::create(std::move(root), foreign_tree->root_ptr());
      CHECK(dynamic_cast<const vm::DataCell*>(root.get()) == nullptr);
    }
    std::optional<vm::NewCellStorageStat> trial;
    result.copy.add(time_action([&] { trial.emplace(workload.base); }));
    result.proof.add(time_action([&] { trial->add_proof(root, workload.usage.get()); }));
    // Verification and destruction are excluded from both timed segments.
    CHECK(trial->get_stat() == workload.expected[index].ordinary);
    CHECK(trial->get_proof_stat() == workload.expected[index].proof);
    CHECK(trial->get_total_stat() == workload.expected[index].ordinary + workload.expected[index].proof);
    CHECK(workload.base.get_stat() == base_ordinary && workload.base.get_proof_stat() == base_proof_stat);
  }
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  Options options;
  try {
    for (int index = 1; index < argc; ++index) {
      const std::string option = argv[index];
      if (option == "--help") {
        std::cout << "bench-native-proof [--rounds 3] [--iterations 3] [--base-accounts 4096]\n"
                     "  [--base-proof-leaves 64] [--mode all|plain_prior|tracked_prior|foreign_usage]\n"
                     "CSV: 8/16/32/64/80/128/192/256/512 updates, 1/4/16 exact checkpoints,\n"
                     "uniform/prefix2 keys; foreign_usage forces fallback at every traversed node.\n"
                     "Copy and proof timers exclude construction, verification and destruction.\n";
        return 0;
      }
      if (++index == argc) {
        throw std::invalid_argument("missing option value for " + option);
      }
      const std::string value = argv[index];
      if (option == "--rounds") options.rounds = parse_unsigned(value);
      else if (option == "--iterations") options.iterations = parse_unsigned(value);
      else if (option == "--base-accounts") options.base_accounts = parse_unsigned(value);
      else if (option == "--base-proof-leaves") options.base_proof_leaves = parse_unsigned(value, true);
      else if (option == "--mode") options.mode = value;
      else throw std::invalid_argument("unknown option: " + option);
    }
    if (options.base_accounts < populations.back()) {
      throw std::invalid_argument("--base-accounts must be at least 512");
    }
    if (options.mode != "all" && std::find(modes.begin(), modes.end(), options.mode) == modes.end()) {
      throw std::invalid_argument("unknown --mode: " + options.mode);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
  std::cout << "base_accounts,base_proof_leaves,distribution,mode,updates,checkpoints,round,iterations,"
               "copy_wall_us_per_checkpoint,copy_cpu_us_per_checkpoint,proof_wall_us_per_checkpoint,"
               "proof_cpu_us_per_checkpoint,trial_wall_us_per_checkpoint,trial_cpu_us_per_checkpoint\n"
            << std::fixed << std::setprecision(3);
  for (const unsigned prefix_bits : {0u, 2u}) {
    const Fixture fixture{options, prefix_bits};
    for (const auto* mode : modes) {
      if (options.mode != "all" && options.mode != mode) continue;
      for (const auto updates : populations) {
        const Workload workload{fixture, updates, mode};
        for (const auto checkpoints : checkpoint_counts) {
          run_once(workload, checkpoints);
          run_once(workload, checkpoints);
          for (unsigned round = 0; round < options.rounds; ++round) {
            Sample total;
            for (unsigned iteration = 0; iteration < options.iterations; ++iteration) {
              const auto sample = run_once(workload, checkpoints);
              total.copy.add(sample.copy);
              total.proof.add(sample.proof);
            }
            const double divisor = static_cast<double>(options.iterations) * checkpoints;
            std::cout << options.base_accounts << ',' << options.base_proof_leaves << ','
                      << (prefix_bits == 0 ? "uniform" : "prefix2") << ',' << mode << ',' << updates << ','
                      << checkpoints << ',' << round << ',' << options.iterations << ','
                      << total.copy.wall_us / divisor << ',' << total.copy.cpu_us / divisor << ','
                      << total.proof.wall_us / divisor << ',' << total.proof.cpu_us / divisor << ','
                      << (total.copy.wall_us + total.proof.wall_us) / divisor << ','
                      << (total.copy.cpu_us + total.proof.cpu_us) / divisor << '\n';
          }
          std::cout.flush();
        }
      }
    }
  }
}
