# Native reconciliation diagnostics

`total.ext_msg_native_reconciliation_diagnostics` attributes work only to the
applied-state reconciliation walk. It is additive; admission, exact canonical
state checks, nonce guards, expiry handling and reservation rules are unchanged.

The older `total.ext_msg_native_reconciliation.sources_advanced` counter also
includes observations made during admission. Dividing it by reconciliation
`account_lookups` does not measure useful reconciliation reads. Use the new,
matching-scope counters below instead.

## Enable stage timing

Set `TON_NATIVE_RECONCILIATION_PROFILE=1` on the validator process and restart it
before a diagnostic run. The default is `0`. Cheap outcome/work counters are
always collected; the per-stage clock reads and timing samples are disabled
unless profiling is enabled. Compare performance arms with the same setting.
The environment must be passed through the container definition when Docker is
used; merely defining a host variable does not change an existing container.

Counters are cumulative for the pool actor lifetime. Use differences between
samples from the same uninterrupted process, covering the same measurement
window. `profile_enabled` is a gauge. Timing `*_max_s` values are lifetime maxima,
not counters to subtract.

| Stage prefix | A sample measures |
| --- | --- |
| `registration` | One pending-target registration pass, including both source maps |
| `grouping` | One snapshot's mapping of tracked sources to exact shard block IDs |
| `manager_wait` | One shard-state request through its awaited completion, including failures |
| `lookup` | One source lookup in the fetched shard's augmented accounts dictionary |
| `unpack` | One source account unpack, including unsuccessful attempts |
| `apply` | One reconciliation-only canonical account application, including stale-LT rejection |
| `wake` | One final callback wake for the snapshot's changed sources, when nonempty |

Each prefix exports `*_samples`, `*_sum_s`, and `*_max_s`. For example,
`delta(lookup_sum_s) / delta(lookup_samples)` gives mean dictionary lookup wall
time. Manager waiting includes suspension and other actor work; it is not CPU
time. These stages are attribution samples, not a complete decomposition of
snapshot residence: source-vector creation, state-header parsing and other
bookkeeping are outside the listed timers. An exception/cancellation can leave
a begun stage without a completion sample.

## Read account outcomes separately from mutations

`apply_calls` counts only successfully decoded native account observations that
reach canonical application during reconciliation. The following outcomes form
an exclusive partition of these calls:

- `apply_errors`: rejected observations; `apply_stale_lt` identifies states older
  than the latest observed canonical logical time.
- `apply_first_observation`: no prior canonical balance was recorded.
- `apply_nonce_advanced`: the effective canonical nonce increased.
- `apply_balance_only_changed`: the nonce did not increase, but balance changed.
- `apply_unchanged`: neither effective nonce nor balance changed.

`apply_balance_increased` and `apply_balance_decreased` overlap the successful
outcomes when a prior balance exists. They retain visibility into incoming
credits and debits even when nonce also advances. A lower input nonce never
reduces the canonical watermark; unchanged classification uses the effective
monotonic nonce.

`apply_effects` counts successful calls whose existing application returned
`true`: account revision changed, messages were purged, reservations were
rebased, or a tail was pruned. **Unchanged account facts can still require work**,
for example removing a message that expired at the new shard time. An unchanged
ratio alone does not justify skipping application.

## Attribute source and reservation work

`register_tracked_visits` and `register_source_visits` count entries visited in
the two existing pending-target registration maps. `group_source_visits` counts
attempted source-to-shard mappings; `group_shards` counts resulting exact shard
groups, including groups subsequently skipped because their tops are unchanged.
Existing `unchanged_top_skips` / `unchanged_source_skips` describe whole-shard-top
skips, not unchanged individual accounts.

`account_lookups` counts actual dictionary queries. `account_empty` is the
subset returning no account slice; it can overlap a later failure. The unpack,
kind and balance failure counters classify disjoint exits before application.
`snapshot_finishes` counts completed snapshot calls, and `snapshot_errors` is
their error subset. A snapshot can apply some sources before returning an error.

`pending_reservations_before_apply_sum` adds each successfully observed source's
pending physical reservation count before purge. This is an occupancy sum,
**not** an exact scan count. `reservation_prefix_entries` counts actual entries
visited in the existing post-purge reservation loop, including entries in a tail
that is subsequently removed. Neither counter adds an extra scan or allocation.
`messages_purged`, `reservation_rebases`, and `tail_prunes` count the actual
mutations made only by these reconciliation calls. Counts refer to physical
message/reservation records; an NTRN record can authorize multiple transfers.

Use lookup/unpack timings together with these matched outcomes to determine
whether account reads dominate. If `apply_sum_s` dominates instead, compare
reservation-prefix entries and mutation counts before considering a dictionary
diff optimization. Skipping a proven unchanged account leaf would still need to
preserve the current canonical time, stale-state guard, incoming balance changes,
expiry processing and reservation checks. No such skipping policy is enabled by
this instrumentation.
