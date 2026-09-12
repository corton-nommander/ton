# Prepared admission with four independent lane owners

Experimental implementation on top of the persistent producer, worker preparation
and canonical journal work. It is disabled by default and has no measured TPS
claim yet. The previous owner prototype lost throughput: its corrected-client
long screen reached 46,119 canonical TPS versus 55,913 and 53,300 controls, with
higher RTT and CPU. This implementation must earn its own repeatable result.
See `doc/native-lane-schedulers-2026-09-12.md` for that prior evidence.

## Activation

Use a newly started validator with the already supported four-lane chain:

```text
TON_NATIVE_ADMISSION_LANE_OWNERS=4
TON_NATIVE_ADMISSION_PREPARE=1
TON_NATIVE_ADMISSION_SNAPSHOT_REFRESH=0
NATIVE_PAYMENT_LANE_DEPTH=2
```

`TON_NATIVE_ADMISSION_LANE_OWNERS=0` preserves the ordinary single-pool path.
The only nonzero accepted value is `4`. The engine rejects owner mode without
preparation, or with independent snapshot refresh enabled. Eight or sixteen
lanes are deliberately unsupported. `TON_NATIVE_LANE_SCHEDULERS=4` additionally
places each owner on its existing lane scheduler group; it does not create
additional verifier workers or validator committees.

The first complete fixed depth-two topology creates four children, including
lane zero. The coordinator holds generic wallets/messages and masterchain
callbacks, but no native account, reservation, nonce watermark or reconciliation
source. A later native mode, lane-policy or topology change latches admission
closed for this process. Children, their last supported state, reservations and
canonical watermarks remain in place; there is no live migration or single-pool
fallback. Existing normal message expiry and explicit cleanup remain available.

## Ownership and exact-state checks

The coordinator's existing verifier actors prepare strict BOCs, native fields
and domain-bound signatures before selecting the common exact spending context.
Children share their ActorIds and create zero preparation/verifier actors. The
original preparation limit remains 4,096 queued physical inputs / 16 MiB; timed
out workers retain their leases until they finish. When that budget is full,
the same owned preparation helper runs synchronously and still routes native
work to the correct child. This fallback is counted separately and does not
expand queued-worker budgets.

Prepared groups preserve original input indices, per-representation wire bounds,
root deduplication and deterministic source/nonce/hash ordering. Each child checks
its applied generation, the shared published generation, exact masterchain block
and state root, current mode/domain/locality and signed expiry. It reads the exact
source shard, then rechecks the generation before applying source state or
reserving/committing. Each owner exclusively changes its own account revisions,
reservations, mempool indexes, canonical watermarks, expiry, reconciliation and
producer views. Canonical authority still comes from exact account state and the
shard-client reconciliation API; local acceptance remains only a source hint.

A short shared publication fence protects synchronous idempotence, source-state
application, reservation and finalization. It never spans an await. Reentrant
inline promise continuations borrow the same thread's existing read fence;
publication defers a read-to-write upgrade. No shared canonical nonce or union of
fork floors is introduced. Callback-local branch floors travel intact to the
owning lane and its persistent producer.

The router does not wrap the whole result join in a new timeout. Every item
retains its original absolute admission deadline, and a sibling that committed
before that deadline must retain its success and broadcast waiter even if another
sibling finishes later. Native single-message requests use the one-item exact
batch path and retain the public idempotence/broadcast-wait result contract.
Generic single-message requests retain their existing wallet/TVM path and repeat
current wire size/depth limits after worker preparation.

Shared RAII permits conserve the original global per-priority mempool cap and
per-priority/address cap across all five actors, including generic/native messages
for the same address. The existing two-half-window checked-message rate limit is
also shared. Promotion acquires its new permit before deleting the old entry;
erasure releases membership even when a BOC or immutable producer view is retained.

Raw-hash and normalized-hash cleanup fan out to the four owners. Source hints
partition by lane. Exact canonical notifications fan out, with each owner's
journal scanning only its own tracked sources. Generic callback snapshots are
staged by the coordinator once, then handed to the lane owner; live generic
entries are forwarded to installed lane callbacks. Synchronous mixed callbacks
retain native-first logical capacity accounting.

## Instrumentation and validation

`total.native_admission_lane_owners` reports the shared worker budget, actual
owner count, topology state, published generation, aggregate mempool, zero-native
coordinator ledger, routed RPC/group counts, physical parent count, synchronous
preparation fallbacks and router join wall time. Each
`native_pool.owner.<n>.identity` reports local versus shared verifier counts,
generation, rejected generation checks, dispatched batch/parent/max-density
counts and owner mailbox wait. Existing native admission, producer and journal
statistics remain namespaced by owner. Router join time includes downstream
service and sibling wait; it is not exclusive coordinator CPU time.

Producer cleanup is visible in each owner's
`total.ext_msg_native_persistent_producer`: `owner_dirty_sources`,
`owner_flush_scheduled`, `inbox_pending_sources`, `inbox_latest_sources`,
`updates_inflight_sources`, `live_sources`, and `live_physical_messages`.
The handoff gauge remains charged from inbox extraction through view application
and callback wake; retirement releases only that producer's membership. Require
repeated zero observations together with empty pool/transport gauges and a
proof-consistent client drain. Idle producer objects, callbacks, cumulative
publication counters and retained canonical nonce watermarks need not be zero.
Reading the added producer gauges uses one lock per lane and shared atomics,
without traversing retained source views.

Authored tests: **17 fixtures total**, not executed before the benchmark build slot.

- `test-native-prepared-owner-coordination` (**7 fixtures**): four-prefix mapping, shared priority
  and address quotas, promotion/rollback/shutdown refunds, reentrant reader/writer
  fence, and exact shared rate-window semantics including concurrent access.
- `test-native-prepared-lane-owners` (**10 fixtures**): actual coordinator/child startup and shared
  workers, lane-zero ownership, real signed mixed-lane batches, duplicates and
  single idempotence; fresh context after held preparation; budget fallback;
  published-generation and held-account-fetch rejection; retained early success
  despite a late generic sibling; latched topology rejection; shared generic
  quota and hash cleanup; all-owner stats; generic current wire limits;
  native-first synchronous callbacks with independent branch floors; hash ordering
  despite reversed preparation completion.

Build and run both targets, existing admission refresh/preparation, scheduler,
producer actor, canonical journal and lane scheduler tests before activation.
During the requested staged experiment, keep one frozen validator image and the
unchanged client image throughout the 2 → 3 → 4 → 1 arms. Enable owners only in
the last arm. Keep broad checkpoint retention off and the logical window fixed
at 65,536 until measured ready work repeatedly hits that cap. Compare measured
phase offered/admitted/canonical TPS, cap hits, physical/logical batch density,
RTT, follower backlog, owner queue time and process CPU against paired controls.
A lower manager dispatch time alone is not a successful end-to-end result.
