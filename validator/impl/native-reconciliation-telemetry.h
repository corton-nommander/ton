#pragma once

#include "native-admission-telemetry.h"
#include "td/utils/common.h"

namespace ton::validator {

// These counters belong only to the applied-state reconciliation walk. The
// historical native_reconciliation_sources_advanced counter also receives
// admission-path progress and cannot be divided by reconciliation lookups.
struct NativeReconciliationTelemetry {
  td::uint64 snapshot_finishes{0}, snapshot_errors{0};
  td::uint64 register_source_visits{0}, register_tracked_visits{0};
  td::uint64 group_source_visits{0}, group_shards{0};
  td::uint64 account_lookups{0}, account_empty{0}, account_unpack_failures{0};
  td::uint64 account_kind_failures{0}, account_balance_failures{0};
  td::uint64 apply_calls{0}, apply_errors{0}, apply_stale_lt{0};
  td::uint64 apply_first_observation{0}, apply_nonce_advanced{0};
  td::uint64 apply_balance_only_changed{0}, apply_unchanged{0}, apply_effects{0};
  td::uint64 apply_balance_increased{0}, apply_balance_decreased{0};
  // The first count is an occupancy sum, not an execution-work count. The
  // second counts actual entries in the post-purge reservation-prefix loop.
  td::uint64 pending_reservations_before_apply_sum{0}, reservation_prefix_entries{0};
  td::uint64 messages_purged{0}, reservation_rebases{0}, tail_prunes{0};
  NativeAdmissionWallTime registration, grouping, manager_wait, lookup, unpack, apply, wake;
};

}  // namespace ton::validator
