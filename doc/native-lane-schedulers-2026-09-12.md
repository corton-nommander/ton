# Lane scheduler domains prototype — 12 September 2026

`TON_NATIVE_LANE_SCHEDULERS=4` enables four execution scheduler domains for the
complete fixed four-leaf basechain topology. The default, `0`, retains the existing
single scheduler and all configured CPU workers. This is **execution scheduling
isolation**, not independent admission/reservation ownership, sharded storage,
additional validators or a measured TPS improvement.

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
inheritance, and checks explicit collation/validation/producer routing. Build and
execution are pending integration; no passing result or performance gain is
recorded yet.

Keep admission preparation, producer and canonical journal flags fixed on both
sides of an A/B comparison. Enable only this scheduler setting in the treatment.
Use repeated 600-second strict-image controls with the same total engine threads,
CPU quota, 20ms coalescing and 32,768/65,536 logical windows. Require complete
canonical proofs and drain, correct placement in all four lanes, stable identities
and useful offered-load headroom. Reject the treatment if main-group admission or
manager queues saturate, downstream backlog grows, or canonical TPS regresses.

## What remains before independent lane admission ownership

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

An implementable next ownership stage is:

1. Extract the sealed prepared-message type and bounded preparation lease into
   a shared preparation service. Decode and verify before routing; retain exact
   message hash/domain binding and original wire-size/deadline checks. The
   ingress router should stop owning native prefix zero or decoding every BOC.
2. Create one `NativeLaneAdmissionOwner` per fixed lane (or explicitly assigned
   lane set), placed with its producer/session domain. Move reservations,
   canonical watermarks, revisions, raw/normalized indexes, per-address limits,
   expiry and reconciliation endpoints exclusively into that actor. This is the
   actual ownership boundary missing from the scheduler prototype.
3. After preparation, select a fresh exact admission context and deliver sealed
   parents plus that context to their owners. Reuse the existing global generation
   commit fence initially; each owner checks the exact current context and source
   revision at a short synchronous reserve/insert/commit boundary. Never hold a
   fence across an await or multiply verifier workers. Preserve ordered per-item
   results and source nonce ordering across mixed-lane RPC batches.
4. Deliver each canonical lane endpoint/journal to its sole owner from the existing
   shard-client-confirmed authority. Include destination balance changes and
   expiry; local candidate acceptance stays a hint. Each owner publishes immutable
   ready views to its lane producer. Bootstrap or missing continuity uses the
   exact-state fallback, not another owner's watermark or competing-fork floors.
5. Retain conserved global mempool permits and explicit per-owner diagnostics.
   The old permit/fence code is a correctness starting point; optimize shared
   locks or lease permits in batches only when profiles show that cost. Keep
   masterchain/generic handling explicit, and fail closed on unsupported ownership
   transitions until there is a tested transfer/drain protocol.

State/storage requests still pass through the shared manager/DB in this design.
Use stage and queue evidence after each step to decide whether a lane state
provider and bounded per-lane storage publication are necessary. Independent
process/server execution would also need authenticated state/data availability,
cross-lane policy and consensus/security design; it is not obtained by adding
admission actors or scheduler groups alone.
