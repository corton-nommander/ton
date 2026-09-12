# Lane scheduler domains prototype — 12 September 2026

`TON_NATIVE_LANE_SCHEDULERS=4` enables four execution scheduler domains for the
complete fixed four-leaf basechain topology. The default, `0`, retains the existing
single scheduler and all configured CPU workers. This flag controls execution
scheduling. Independent admission/reservation ownership is implemented separately
by the [prepared lane-owner prototype](native-prepared-lane-owners-2026-09-12.md).
Neither flag shards storage or adds validators; their performance result remains
pending.

The engine conserves its configured CPU-worker total. Four workers remain in the
main scheduler; the rest are divided evenly across four lane schedulers, assigning
remainder workers to the lower lane indexes. Examples:

| Engine CPU workers | Main | Lane 0 | Lane 1 | Lane 2 | Lane 3 |
|---:|---:|---:|---:|---:|---:|
| 8 | 4 | 1 | 1 | 1 | 1 |
| 16 | 4 | 3 | 3 | 3 | 3 |
| 17 | 4 | 4 | 3 | 3 | 3 |
| 48 | 4 | 11 | 11 | 11 | 11 |

The enabled mode requires at least eight configured CPU workers. TD additionally
creates one I/O/event-loop thread for each added scheduler group. Those four
threads and existing RocksDB/signature/helper threads are **not** included in the
CPU-worker conservation claim. This change does not pin OS threads to CPU cores
or NUMA nodes and does not increase a container's CPU quota.

The manager publishes whether its current masterchain state contains exactly the
four basechain leaves `2000…`, `6000…`, `a000…`, `e000…`. Their stable scheduler IDs
are 1–4; scheduler 0 handles the main engine/manager and unsupported shards.
Bridge actors use that mapping. Their ManagerFacade and consensus BusRuntime
actors inherit the bridge's scheduler, including BlockProducer, StateResolver and
BlockValidator. Collator and ValidateQuery creation also use explicit mapping,
so a manager-side call cannot accidentally create them on the main scheduler.
The persistent producer integrates through the same
`native_lane_actor_options(..., NativeLaneActorKind::producer)` helper.

Unsupported, incomplete, split/merged and eight-leaf topologies route **new**
placements to main. Existing actors retain their assigned scheduler until normal
session retirement; no live actor migration or reservation transfer is attempted.
Scheduler groups are fixed at engine startup, so unsupported-topology fallback
can leave lane workers idle and main with only four CPU workers. Disable the
feature and restart to restore the full original shared scheduler on such a
chain. Do not enable it on the current eight-lane diagnostic chain expecting
unchanged resource distribution.

`getstats` exposes `total.native_lane_schedulers`: configured CPU workers per
group, current topology support, topology/unsupported update counts and cumulative
Bridge/Collator/Validator/Producer placements by group. Placement counters are
actor creations, not active actor counts, transactions or CPU use. Compare the
per-group named OS worker threads and actor CPU/queue diagnostics during the
measurement. Startup logging reports the same layout.

The focused `test-native-lane-scheduler` target checks disabled behavior, total
worker conservation across 8–127 workers, uneven remainders, invalid settings,
exact topology mapping and unsupported fallback. Its runtime test creates real
actors on all five scheduler groups, verifies ordinary child and BusRuntime
inheritance, and checks explicit collation/validation/producer routing.

The frozen `52a20df0` implementation passed **144 focused cases**, the native
publication gate's **21 invocations**, and all **seven frozen-runtime target
checks**. These are recorded executions, not newly rerun tests. The exact binary,
test and log hashes are in
[integration verification](../build/benchmarks/native-lane-pipeline-20260912/integration-verification-52a20df0.json)
and the [frozen runtime gates](../build/benchmarks/native-lane-pipeline-20260912/frozen-runtime-gates-52a20df0.json).
The later split-startup build `095bc0de` repeated the 144 focused cases,
21 publication-gate invocations and seven frozen-runtime checks. Its existing
mock-manager ordinary-idle regression also passed; a real fresh zero-ingress
split transition remains a separate pending gate. See the
[cycle report](native-lane-pipeline-cycles-2026-09-12.md#validation-and-next-context).

Keep preparation, producer and journal flags fixed across a comparison. A
scheduler-only experiment changes only this setting. The current stage1 experiment
instead tests **owners4 plus domains4 together**, against owners0/domains0; it
cannot isolate either feature's contribution. Both arms require preparation1,
producer1 and journal0, the same total engine threads/CPU quota, 20ms coalescing
and 32,768/65,536 logical windows. Require complete canonical proofs and drain,
correct placement in all four lanes, stable identities and useful offered-load
headroom before a capacity claim.

The corrected comparison requires generator `bc15f976` and MyLocalTonDocker
observer `e04b96f` on both sides; see [client compatibility](native-prepared-lane-owners-2026-09-12.md#client-and-observer-compatibility).
The initial incompatible-client pair and the later lane-imbalanced corrected
control are excluded from feature selection; the tolerance was not widened.

Performance result: **not established**. The first corrected control failed the
fixed lane-balance gate after complete proof/drain; a new balanced context is pending. No scheduler or combined owner/domain
TPS improvement is claimed. The result and final profile decision will be recorded
in the [12 September cycle report](native-lane-pipeline-cycles-2026-09-12.md).

## Implemented ownership and prior evidence

The prior `TON_NATIVE_POOL_OWNERS` prototype was actual pool ownership: commit
`e4192789` (duplicate lineage `ed3c86c8`) partitioned source reservations,
watermarks/revisions, message indexes, expiry, reconciliation and scheduling.
It already conserved the total verifier count. The corrected-client evidence at
`80816c32` observed 55,913 / 46,119 / 53,300 canonical TPS for control/four-owner/
control; the candidate used 14.092 CPU cores versus 10.311, increased RTT from
193 to 341ms and incurred 5.355% canonical backpressure. It was not promoted.
These observations implicate additional downstream/queue pressure, but the
candidate timing populations and overlapping wall timers do not establish an
exclusive causal CPU attribution.

Do not cherry-pick that complete runtime treatment. Reuse its topology/routing,
quota/refund, exact-generation fencing, original-deadline and branch-floor tests.
Preserve the later `2cf2b083`/`7914d3d4` correction: a router must not discard a
committed sibling's success by applying a second timeout to result collection.
Admission may not commit after its deadline even when returning collected results
later. The previous client classifier omission was corrected before the long
losing screen; it is not an explanation for that screen's regression.

The new [prepared lane-owner implementation](native-prepared-lane-owners-2026-09-12.md)
now provides the ownership boundary. The router prepares strict, sealed messages
using its existing bounded worker pool, then selects a fresh common exact spending
context. Four actual child owners, including lane zero, exclusively own native
reservations, watermarks, indexes, expiry, reconciliation and producer publication.
The coordinator retains generic/masterchain handling and has no native ledger.

The implementation retains the shared generation commit fence, global quota
permits, original per-item deadlines, ordered results and callback-local branch
floors. Workers are shared rather than multiplied. Canonical reconciliation still
comes from the shard-client-confirmed authority, with the exact-state fallback
when journal continuity is unavailable. Unsupported ownership transitions latch
admission closed and retain existing state; no live ownership migration is
implemented. The prior prototype is historical evidence, not this implementation's
performance result.

State/storage requests still pass through the shared manager/DB in this design.
Use stage and queue evidence after each step to decide whether a lane state
provider and bounded per-lane storage publication are necessary. Independent
process/server execution would also need authenticated state/data availability,
cross-lane policy and consensus/security design; it is not obtained by adding
admission actors or scheduler groups alone.
