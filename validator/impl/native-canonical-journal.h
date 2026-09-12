/*
    This file is part of TON Blockchain Library.
    TON Blockchain Library is free software under the GNU Lesser General
    Public License, version 2 or (at your option) any later version.
    See LICENSE.LGPL in the source distribution for details.
*/
#pragma once

#include <vector>

#include "ton/ton-types.h"
#include "vm/cells.h"
#include "native-admission-telemetry.h"

namespace ton::validator {

// These are successful canonical reconciliation endpoints, never candidate
// acceptance hints. One endpoint per current shard is retained by the pool.
struct NativeCanonicalAccountObservation {
  StdSmcAddress address;
  td::uint64 nonce{0};
  td::uint64 balance{0};
};

struct NativeCanonicalJournalEndpoint {
  BlockIdExt masterchain_block;
  RootHash masterchain_root;
  BlockIdExt shard_block;
  RootHash state_root;
  td::Ref<vm::Cell> accounts;
  UnixTime gen_utime{0};
  LogicalTime gen_lt{0};
  // Sorted observations only for sources actually checked at this endpoint.
  // Newly pending sources must bootstrap from the exact successor dictionary.
  std::vector<NativeCanonicalAccountObservation> observations;
};

struct NativeCanonicalJournalTelemetry {
  td::uint64 attempts{0}, completed{0}, bootstrap{0}, continuity_fallbacks{0};
  td::uint64 diff_fallbacks{0}, budget_fallbacks{0}, topology_evictions{0};
  td::uint64 diff_changed_accounts{0}, diff_tracked_changed{0};
  td::uint64 reused_observations{0}, lookup_bootstraps{0}, unchanged_apply_skips{0};
  td::uint64 reservation_checks{0}, apply_required{0}, failed_shards{0};
  td::uint64 retained_sources{0}, peak_retained_sources{0};
  NativeAdmissionWallTime diff;
};

}  // namespace ton::validator
