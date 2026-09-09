#include "native-parent-metadata-fixtures.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <set>
#include <vector>

namespace {
using block::NativeTransferBatch;
using Metadata = NativeTransferBatch::ExternalMessageMetadata;
using Clock = std::chrono::steady_clock;
volatile td::uint64 observed = 0;

std::vector<Metadata> full_metadata(const NativeTransferBatch& batch) {
  std::vector<Metadata> result;
  result.reserve(batch.runs.size());
  for (const auto& run : batch.runs) {
    result.push_back({run.external_hash().move_as_ok(), run.src, run.first_nonce,
                      static_cast<td::uint32>(run.outputs.size())});
  }
  return result;
}

template <class F>
void sample(const char* stage, const char* workload, unsigned version, std::size_t count, std::size_t parents, std::size_t boc_bytes,
            unsigned repetitions, F function) {
  observed = function();  // One untimed warm-cache iteration for every stage.
  std::vector<double> times;
  for (unsigned index = 0; index < repetitions; ++index) {
    const auto start = Clock::now();
    const auto value = function();
    const auto end = Clock::now();
    observed = value;
    times.push_back(std::chrono::duration<double, std::micro>(end - start).count());
  }
  std::sort(times.begin(), times.end());
  double total = 0;
  for (double time : times) total += time;
  std::cout << std::setprecision(12) << "{\"schema\":\"native-parent-metadata-micro-v1\",\"version\":" << version
            << ",\"logical_updates\":" << count << ",\"parents\":" << parents << ",\"boc_bytes\":" << boc_bytes
            << ",\"workload\":\"" << workload << "\",\"stage\":\"" << stage << "\",\"repetitions\":" << repetitions
            << ",\"mean_us\":" << total / repetitions << ",\"median_us\":" << times[times.size() / 2]
            << ",\"min_us\":" << times.front() << ",\"max_us\":" << times.back()
            << ",\"mean_ns_per_logical_update\":" << total * 1000 / repetitions / static_cast<double>(count) << "}\n";
}

void benchmark(unsigned version, std::size_t count, unsigned repetitions, bool repeated_outputs) {
  auto batch = native_metadata_test::direct_batch(static_cast<td::uint8>(version), count);
  if (repeated_outputs) {
    for (auto& run : batch.runs) std::fill(run.outputs.begin(), run.outputs.end(), run.outputs.front());
  }
  auto root = native_metadata_test::serialize(batch);
  auto boc = vm::std_boc_serialize(root, 31).move_as_ok();
  auto full = NativeTransferBatch::unpack(root).move_as_ok();
  auto expected = full_metadata(full);
  auto projection = NativeTransferBatch::unpack_external_metadata(root).move_as_ok();
  CHECK(expected.size() == projection.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    CHECK(expected[index].hash == projection[index].hash && expected[index].source == projection[index].source &&
          expected[index].nonce == projection[index].nonce && expected[index].logical_count == projection[index].logical_count);
  }
  auto measure = [&](const char* name, auto function) {
    sample(name, repeated_outputs ? "repeated_output" : "distinct_outputs", version, count, batch.runs.size(),
           boc.size(), repetitions, function);
  };
  // Stages overlap; these are isolated costs, not additive CPU attribution.
  // The parse comparisons start from the same already decoded cell graph.
  measure("boc_decode_only", [&] {
    auto decoded = vm::std_boc_deserialize(boc).move_as_ok();
    return decoded->get_depth();
  });
  measure("strict_full_unpack", [&] {
    return NativeTransferBatch::unpack(root).move_as_ok().entries.size();
  });
  measure("strict_parent_projection", [&] {
    return NativeTransferBatch::unpack_external_metadata(root).move_as_ok().size();
  });
  measure("flatten_entries_only", [&] {
    return NativeTransferBatch::flatten_runs(full.runs).move_as_ok().size();
  });
  measure("derive_accounts_only", [&] {
    std::set<ton::StdSmcAddress> known;
    std::vector<ton::StdSmcAddress> accounts;
    accounts.reserve(full.entries.size() * 2);
    for (const auto& entry : full.entries) {
      if (known.insert(entry.transfer.src).second) accounts.push_back(entry.transfer.src);
      if (known.insert(entry.transfer.dst).second) accounts.push_back(entry.transfer.dst);
    }
    return accounts.size();
  });
  measure("rebuild_parent_hashes_only", [&] { return full_metadata(full).size(); });
  measure("full_unpack_and_parent_hashes", [&] {
    return full_metadata(NativeTransferBatch::unpack(root).move_as_ok()).size();
  });
  measure("boc_and_full_metadata", [&] {
    return full_metadata(NativeTransferBatch::unpack(vm::std_boc_deserialize(boc).move_as_ok()).move_as_ok()).size();
  });
  measure("boc_and_parent_projection", [&] {
    return NativeTransferBatch::unpack_external_metadata(vm::std_boc_deserialize(boc).move_as_ok()).move_as_ok().size();
  });
}
}  // namespace

int main(int argc, char** argv) {
  unsigned repetitions = 7;
  std::size_t maximum = NativeTransferBatch::max_entries;
  for (int index = 1; index < argc; ++index) {
    const std::string option(argv[index]);
    if (option == "--help") {
      std::cout << "Usage: bench-native-parent-metadata [--repetitions 1..1000] [--max-updates 64..65536]\n"
                   "Emits JSON lines for v5/v6 NTRN-16 batches at 64,256,1024,4096,16384,65536 updates.\n"
                   "Runs repeated-output production-like and distinct-output workloads separately.\n"
                   "Warm-cache wall-time microbenchmark; stages overlap and exclude crypto signature verification.\n"
                   "This measures decoding/materialization, not validator TPS or network capacity.\n";
      return 0;
    }
    if ((option != "--repetitions" && option != "--max-updates") || index + 1 == argc) return 2;
    const std::string value(argv[++index]);
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos || value.size() > 6) return 2;
    const auto parsed = std::strtoul(value.c_str(), nullptr, 10);
    if (option == "--repetitions") {
      if (parsed < 1 || parsed > 1000) return 2;
      repetitions = static_cast<unsigned>(parsed);
    } else {
      if (parsed < 64 || parsed > NativeTransferBatch::max_entries) return 2;
      maximum = parsed;
    }
  }
  for (unsigned version : {5, 6}) {
    for (bool repeated_outputs : {true, false}) {
      for (std::size_t count : {64u, 256u, 1024u, 4096u, 16384u, 65536u}) {
        if (count <= maximum) benchmark(version, count, repetitions, repeated_outputs);
      }
    }
  }
}
