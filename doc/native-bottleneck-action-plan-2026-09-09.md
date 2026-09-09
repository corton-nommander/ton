# Native throughput bottlenecks and proposed actions — 9 September 2026

Status: steps 1–4 approved by the user on September 9; implementation and isolated
desktop measurements are in progress. The conditional architectural redesign still
requires a separate decision. The evidence below predates these new experiments.

The recommendation remains to improve admission and useful-work delivery before
rewriting native execution. External-work wait is the largest measured collation
delay, but it is a symptom with several causes, not a separate CPU bottleneck.
The evidence does not establish that the current architecture cannot go faster,
or that removing retries alone can turn 50–62k into 200k TPS.

## Evidence scope

- [Four completed desktop tests](native-desktop-admission-benchmark-2026-09-08.md):
  600 measured seconds each, 20 ms coalescing, four lanes, fixed images, full
  proof/drain completion, approximately 61.2–62.0k canonical logical transfers/s.
- Saved remote dashboard data: approximately 48.6–51.7k TPS in selected sustained
  portions, external wait 101–169 ms/block. See
  `build/benchmarks/dashboard-20260908/report.md` and the MyLocalTonDocker
  `benchmark/remote/reports/native-chart-scaling-20260908.md` report. These are
  illustrative windows, not independently matched generator arms.
- The reported eight-lane chain produced more blocks without higher aggregate
  TPS. Its final matching Server A profile and Server B result are not available
  locally. On September 9 the public dashboard connection failed, so no fresh
  remote bottleneck measurement is claimed.
- Desktop and remote numbers are separate workloads/hardware observations.
  Unpaced desktop offered/canonical ratios were approximately 1.0; the measured
  image's capacity classification bug is corrected locally in `3dffcd38`, but
  its old raw capacity flags must not establish an independently overdriven limit.

## Three highest-priority bottleneck candidates

### 1. Shared admission work, redundant requests and snapshot churn

The cache-on desktop arm spent 113.3 ms average in admission, including a 54.7 ms
shard-state await and a 94.7 ms verification stage. These wall stages overlap;
verification includes queue/group waits and is not 94.7 ms of signature CPU.
Changed snapshots rejected 18.18% of completed physical inputs. ExtMessagePool
also appeared highly occupied in actor samples; actor occupancy is not an OS
per-thread CPU profile and needs confirmation on the physical host.

New measured-interior reanalysis finds **56,456 pool-to-manager shard fetches,
3,299 cache fills and 52,971 fill races (93.8% of fetches)** in the cache-on arm.
The same ratio is 92–95% in the other arms. Exact results and intervals are saved
in [the fetch review](benchmarks/results/native-admission-shard-fetch-review-20260909.json).
`ExtMessagePool` caches completed views but does not share in-flight requests
between batch coroutines. `ValidatorManagerImpl::wait_block_state` already shares
the underlying state load: this is redundant requests, callbacks and header/view
work, not evidence of repeated disk reads. The percentage is not a TPS estimate.

Cache tuning alone is largely exhausted: configuration extraction was only
0.0117 ms uncached, and a 99.91% cache hit rate gave only an unconfirmed 1.31%
observed TPS difference. Smaller admission windows reduced not-ready responses
624,249 → 333,984 and timeouts 89 → 0, yet TPS changed 61,950 → 61,844.
Retry reduction therefore helps latency/reliability but is not a demonstrated
throughput cure by itself.

The same interior interval also contains 11,884,177 canonical-reconciliation
account lookups, in addition to admission account reads. The 2,187,509
`sources_advanced` counter also includes advances from admission paths, so it is
not a matching denominator for those reconciliation reads. The lookup volume
identifies another shared-actor profiling target, not a redundant-read percentage.

### 2. Gaps between admissible work and collator consumption

Remote external-work waits were 101–169 ms/block; the desktop control had 111.7 ms
external wait against 31.7 ms collator work. In the remote 05:18 UTC sample the
wait split was 51.0 ms first work, 36.1 ms refill and 15.6 ms after commit.
Native execution was 3.75 ms and native commit 12.78 ms in that same sample.

These waits include queue handoff and deliberate bounded packing; they overlap
the upstream admission pipeline. They cannot be added to admission residence
or interpreted as recoverable CPU percentages. The missing discriminant is
whether ready, branch-valid producer work exists while the collator waits.
Cutting waits blindly can produce smaller blocks and more fixed work per transfer.

### 3. Shared block commit, persistence and propagation costs

The eight-lane plateau makes shared work a strong scaling hypothesis. ArchiveSlice
approached one actor execution equivalent; KeyValue and Overlay actor categories
were prominent. Those categories can aggregate several actor instances and do
not prove disk saturation. Native commit already exceeded native execution in the
remote sample. More small blocks amplify setup, state finalization and storage.
Block signature verification also created about 53–70 helper threads/s in desktop
arms, but its exclusive cost has not been shown to dominate. Admission signature
workers are already persistent actors; these are different paths.

This third priority has weaker causal evidence than admission/wait measurements.
Profile CPU stacks, actor queue delay, storage latency/compaction and allocation
before selecting a persistence rewrite or reusable block-verification executor.

## Proposed delivery sequence

1. **Establish an attributable physical-host baseline.** Obtain the final remote
   result and matching profiler output. Add only missing timestamps/gauges across
   admission entry, exact-state await, verification, reservation, producer-ready,
   queue publication and collator pop. Correlate queue age and producer-pending
   state; record OS CPU/runqueue and I/O evidence in separate profiling captures.
   Include the capacity-reporting fix in newly prepared test images. Check client
   actor queues, actual credits and offered load too: less than one average client
   CPU does not exclude a single-actor or pacing bottleneck.
2. **Share exact-state requests at admission.** Introduce a bounded in-flight
   shard-view request table, keyed by the full pinned masterchain and shard block
   identities. Each caller keeps its original deadline/cancellation; one cancelled
   waiter must not cancel other valid waiters. Check returned identity/root before
   publishing, and prevent old generations from filling current caches. Compare
   manager requests per exact state, callbacks, admission latency and canonical TPS.
3. **Refresh changed snapshots once, then improve delivery.** If churn remains
   material, allow one refresh/revalidation within the original request deadline.
   Reuse only immutable decode/hash/signature evidence whose content, key and
   domain still match. Recheck activation, topology/locality, nonce, balance,
   account revision, reservation and insertion/commit guards. Continued changes
   still fail. Separately test batched callbacks and immediate delivery when
   branch-valid ready work exists. Tune packing only with pending-work evidence.
4. **Act on the remaining CPU profile.** Move proven expensive pure preparation
   off the shared admission actor; keep per-source state mutation single-owner.
   Test reusable bounded block-signature workers only if thread costs matter.
   Persistence already has batched append support; identify remaining serialization
   or small writes before extending batching. Preserve ordering, durability and
   proof availability. These are separate experiments, not one patch.

For steps 2–4, use one feature per intermediate commit, prepare images before
measurement, run a 600-second screen, then repeat promising versions A/B/B/A.
Keep 20 ms coalescing, 10 connections, run size 16 and fixed topology as controls.
Retain the current eight-lane production database; use a separate four-lane
reference. Do not rebuild/restart a validator during a measured arm. A promotion
requires a repeatable canonical TPS improvement beyond control variation, complete
proof/drain, and acceptable latency/resource cost. A latency improvement can be
reported separately without calling it a TPS win. Retain all failed results.
Before performance trials, verify concurrent exact-key joins, waiter cancellation
and deadlines, stale-generation completion, wrong-root rejection, repeated snapshot
changes, same-source reservations and competing-fork behavior with focused tests.

## Conditional architectural redesign

If the shared actor remains a measured ceiling after the preceding changes,
propose **admission and scheduling owned independently per lane/source partition**:
a small ingress router dispatches to independent state caches, verification
queues, reservations and ready-work queues. Each source has exactly one owner;
global memory limits and canonical updates remain coordinated. Splits/merges need
an explicit ownership handover. Adding worker threads around one mutating pool
does not provide this independence.

If archive/state work then serializes those pipelines, partition the corresponding
state/archive processing by lane, with bounded ordered publication of canonical
results. If one machine still cannot meet 200k, evaluate assigning disjoint shard
execution/validator groups to multiple hosts. Adding replicas which all validate
the same lanes does not divide the work. Distributed ownership/committee changes
require a separate consensus, recovery and security design before implementation.

Preserve exact-parent nonce floors, per-source canonical watermarks and fork
isolation, full signature/state validation, ordered per-item batch results,
durability and proof accounting throughout. Never infer nonce floors from the
union of forks or replace correctness checks with unconditional retries.

There is no evidence-based 4x prediction. At 200k logical TPS and 16 transfers per
parent, ingress must sustain 12,500 new signed parents/s before retries, while
execution, persistence and proof observation each keep pace. To claim capacity,
calibrate genuine excess unique offered load with bounded backlog and drain
headroom; do not count retry attempts as additional offered work.

Leave sixteen lanes, candidate timeout reductions, larger client connection
sweeps, broader checkpoint retention and a native-execution rewrite deferred.
The staged-trie threshold remains dormant (observed updates <=256 versus 512),
so more trie workers are not a demonstrated intervention either.

Approved scope: steps 1–4 with measurement gates; keep the conditional architectural
redesign for a separate decision after those results.
