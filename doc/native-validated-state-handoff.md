# Validated state handoff experiment

`TON_NATIVE_VALIDATED_STATE_HANDOFF=1` lets successful block validation pass
its already-computed next shard state to the manager for the later acceptance
of the exact same `BlockIdExt`. The default is `0`. Only literal `0` and `1`
are accepted; malformed values fail startup or first use rather than silently
selecting a mode.

Validation already applies and verifies the block's Merkle update, checks the
resulting state hash against the hash committed by that update, and validates
the state header. With the experiment enabled, it creates a `ShardState` over
that verified root and sends it to a manager-local transient cache only when
the root is fully materialized. Full-collated-data validation can produce a
virtualized Merkle-proof root; those roots always follow the ordinary
reconstruction path because CellDb intentionally does not persist virtualized
cells. Validation reports success after the manager acknowledges insertion. If
acknowledgement does not arrive within one second, validation still reports its
completed success and acceptance follows the ordinary reconstruction path on a
cache miss.

The cache is separate from the readable block-state cache. Entries are keyed
by the full block identity, consumed once, limited to 32, and expire after 30
seconds. The manager accepts an insertion only when the state carries the same
block identity, its root is non-null and non-virtualized, and its actual root
matches the independently supplied validated state hash. Before consuming an
entry, it also requires a non-null incoming `BlockData` root whose actual hash
matches the full block ID. Any mismatch or miss runs the original
`store_block_state_from_data` path.

Validation results backed by a virtualized Merkle-proof root are never handed
off. CellDb deliberately does not persist virtualized roots, so both validation
and the manager reject them and leave acceptance on the original materialized
state-reconstruction path.

Acceptance verifies the actual previous-state root against the old root in the
block update before asking the manager to store either representation. The
handoff stores with an empty `StoreCellHint`: validation-time hints are not
retained because CellDb garbage collection can invalidate their assumption
that every referenced previous-state cell remains present. CellDb therefore
performs its ordinary safe presence discovery while avoiding the second Merkle
update and state reconstruction.

`getstats` exposes `total.native_validated_state_handoff` with:

| Field | Meaning |
| --- | --- |
| `enabled` | Effective process configuration. |
| `capacity` | Fixed entry bound, currently 32. |
| `entries` | Current endpoint gauge. |
| `inserts` | Validated states admitted to the transient cache. |
| `hits` | Exact entries consumed by a state-store request. |
| `misses` | Exact lookup attempts without a live entry. |
| `evictions` | Oldest entries removed at the capacity bound. |
| `expirations` | Unconsumed entries removed after the TTL. |

A hit is counted before the downstream durable CellDb store finishes, so it
proves that reconstruction was bypassed, not that acceptance ultimately
completed. Evaluate ordinary state-store errors, proof completion and drain
alongside this telemetry. Compare `0/1` in a same-image A/B/B/A with every
signature, lane, mailbox and durability setting fixed. A useful treatment
should show nonzero hits, few or no capacity evictions, lower state-rebuild
work, complete canonical proofs, and no regression in state-store time or
memory.

`test-validated-state-handoff` covers strict parsing, distinct block/state
roots, actual `BlockData` root matching, exact consume-on-hit behavior, capacity
eviction and expiry. The validator build additionally checks the manager,
validation and acceptance integration.
