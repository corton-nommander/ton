# Prepared admission with four independent lane owners

The four-owner implementation is present and passed the frozen correctness gates.
It builds on the producer and preparation boundaries; the canonical journal is
optional and remains off in the current comparison. Owners are disabled by default,
and their performance decision remains unestablished after the corrected control
failed lane balance. A fresh balanced comparison is pending. The previous owner prototype lost throughput: its corrected-client
long screen reached 46,119 canonical TPS versus 55,913 and 53,300 controls, with
higher RTT and CPU. This implementation must earn its own repeatable result.
See [lane scheduler domains](native-lane-schedulers-2026-09-12.md) for that prior
evidence.

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

## Client and observer compatibility

The prepared-owner path requires the generator fix in `bc15f976` for meaningful
NTRN congestion measurements. It recognizes only these complete server code-651
messages for a concrete signed-run task:

- `native pool owner has not applied the current admission state; retry`
- `native pool routed admission snapshot changed; retry`

They describe a generation or exact routed-state race before admission commit.
The client uses its existing bounded snapshot/revision retry and preserves the
window, exact parent bytes and nonce. These responses grant no admission or
canonical proof credit. Scalar messages, actual pressure, timeouts, topology
errors and unknown availability errors retain their prior behavior; the original
retry horizon and drain deadline still apply. The fix passed 66 policy cases
and the production-worker/TL callback regression. The latter uses synthetic
preserved bytes to test accounting and identity, not cryptographic validity or
remote throughput.

MyLocalTonDocker `e04b96f` fixes both cleanup and profiling parsers for the
engine's `topology_ready:true` / `topology_failed:false` fields. Only declared
topology flags accept boolean tokens; required numeric cleanup gauges still
reject them. All 53 Python cases passed, including selected raw owner snapshots.
The corrected client comparison uses this observer and one newly frozen client
image for every arm. Updating source files alone cannot replace an exported or
running client image.

The initial owner pair remains excluded from feature selection: its old client
misclassified the new generation response as ordinary congestion, and its wrapper
failed while parsing topology booleans. A separate reviewed proof/cleanup handoff
permits the corrected comparison to use the same settled chain; it does not
rewrite the old invalid benchmark or substitute that arm as a new control.
See the [cycle report](native-lane-pipeline-cycles-2026-09-12.md) for exact images,
validation receipts and corrected results.

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

The two ownership targets passed **17 fixtures total** in the frozen gate.

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

Performance result: **not established**. The first corrected control failed the
fixed lane-balance gate after complete proof/drain; a new balanced context is pending. The comparison tests owners4
plus domains4 against owners0/domains0, with preparation1 on both sides as an
owner dependency. Preparation alone was inconclusive and remains off in the
selected reference; the journal showed no demonstrated TPS gain and also remains
off. A conditional owner/domain result must therefore be distinguished from a
complete-profile comparison against that reference. No owner/domain throughput
or capacity improvement is claimed. See the [12 September cycle report](native-lane-pipeline-cycles-2026-09-12.md).

Keep one frozen validator image and the unchanged client image within every
paired comparison. The stage1 comparison retains producer1, journal0 and broad
checkpoint retention off on both sides. Keep the maximum logical window fixed
at 65,536 until measured ready work repeatedly hits that cap. Compare measured
phase offered/admitted/canonical TPS, cap hits, physical/logical batch density,
RTT, follower backlog, owner queue time and process CPU against paired controls.
A lower manager dispatch time alone is not a successful end-to-end result.
