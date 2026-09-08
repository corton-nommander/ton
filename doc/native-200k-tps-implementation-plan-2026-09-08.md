# Plan: from approximately 50k to 200k canonical native TPS

Date: 2026-09-08. This is an implementation and measurement plan, not a new benchmark result. No remote load, deployment, hardware purchase, or validator configuration change was performed for this plan.

Implementation update: MyLocalTonDocker commit `b5f5e76` extends the helpers through depth 3 and makes eight lanes the fresh `.env.physical` and remote-preset default. Exports still inherit the actual genesis topology. The subsequent user-reported eight-lane run increased block production to approximately 40–50 blocks/s while canonical native TPS remained approximately 50k: no reported throughput gain. Its exact measurement window, effective settings and final proof/drain record were not supplied, and the public dashboard was unreachable during follow-up. See the [eight-lane result and revised priorities](native-eight-lane-plateau-2026-09-08.md) and [remote client guide](native-remote-client-guide.md).

## Target and evidence

The target is **200,000 canonical logical native transfers/s sustained for at least 600 seconds**, using the current lane-local workload and signed-run semantics (at most 16 logical transfers per NTRN parent), complete proof-checked cohorts and a successful drain. Preserve separate figures for physical signed parents, admission attempts and RPCs. At 200k logical TPS, full 16-transfer runs mean 12,500 new signed parents/s before retries; this is not 200k independent signatures/s or general TVM TPS.

The September 8 public-dashboard investigation found sustained portions around 48.6–51.7k canonical TPS. They were not independently matched to connection counts because the dashboard had no test-run records. Relevant evidence is saved in MyLocalTonDocker at `benchmark/remote/reports/native-chart-scaling-20260908.md` and its evidence archive.

Observed external wait averaged 101–169 ms per accepted block in those portions. At 05:18 UTC it comprised approximately 51 ms waiting for first work, 36 ms for refill and 16 ms after commit, compared with 3.75 ms native execution and 12.78 ms native commit per block. These timers are attribution clues, not additive, independent percentages of whole-server CPU use. The data does not distinguish client underfeeding from slow admission or producer scheduling.

All 188,726 reported staged-trie builds used one worker, and updates never exceeded 256; the parallel threshold is 512. Earlier proof/trie component improvements did not establish repeatable eligible end-to-end gains. Existing branch-local nonce floors, native scratch changes and retry corrections are already part of the baseline; do not count them again as proposed gains.

A fourfold improvement from four timing parameters alone is not supported by this evidence. Prioritize wasted admission work, continuous useful ingress and state/validation cost; use more independent shards and distributed compute if a single host reaches its measured ceiling.

## What the proposed timing parameters control

| Parameter | Meaning in this deployment | Initial action |
| --- | --- | --- |
| `SIMPLEX_TARGET_RATE_MS=300` | Masterchain pacing remains active. With `TON_SIMPLEX_MAX_TPS=1`, shardchains publish successful work immediately and skip normal target-rate pacing. | Keep 300. Faster masterchain turnover can increase admission snapshot invalidations. Consider a separate 300/500 ms experiment only after measuring anchoring latency and snapshot churn. |
| `TON_SIMPLEX_MAX_TPS_CANDIDATE_TIMEOUT_MS=8000` | Outer failure bound. Local work gets `min(0.8 * 8000, 8000 - 1000) = 6400` ms. | Keep 8000 while ordinary candidates are not deadline-limited. It is not an eight-second inter-block delay. |
| `TON_SIMPLEX_MAX_TPS_FINALIZE_RESERVE_MS=1000` | Reserve inside the local work budget, not a one-second sleep. Together with the 100 ms fragment-start guard it leaves 5300 ms for starting intake. | Keep 1000. Only test a lower reserve against measured sealing-tail latency and actual deadline seals; do not sacrifice completed candidates. |
| `NATIVE_LOAD_SUBMIT_COALESCE_MS=20` | Maximum leading-edge wait for fresh batchable work. A batch filling physical or available logical credit can dispatch earlier; retries/repairs/drain bypass the fresh-work wait. | Compare 20, 10 and 5 ms individually. Shorter waits may increase RPC overhead and shrink batches. |

Implementation: `validator/consensus/block-producer.cpp`, `validator/consensus/utils.{h,cpp}`, and `lite-client/native-load-generator.cpp`.

## Priority 0 — make the result attributable and repeatable

Deliver a baseline recorder and reliable stop/drain/restart procedure before a large parameter sweep:

- Save source revisions, immutable image IDs, effective settings, workload/account population, exact measurement times, per-lane canonical TPS, final proof/drain status and validator process identity. Publish test-run identifiers to Session Stats so charts can be assigned to their actual settings.
- Record offered unique logical transfers, admission, canonical production, physical batch density, actual congestion window, query-credit stalls, source holds, RTT and retry reasons. Distinguish snapshot changes from account-revision changes, and measure time spent in each admission stage.
- Sample CPU by thread, cgroup throttling, VM steal time, effective frequency, run-queue delay, NUMA placement, memory/cache pressure, database I/O and directional interface bytes on A and B. Use short profiling captures separately from promotion runs.
- Report the lock-owner PID and provide a bounded stop command that lets the generator drain before stopping its wrapper. Preserve incomplete artifacts and explicit source-reuse checks.

Use prebuilt strict image reuse for each paired comparison. Build outside timed runs. When validator code or environment must change, use matched restart/settling procedures on both versions and record process identities; do not accidentally restart one control through an image rebuild. Use ten-minute A/B/B/A measurements to confirm promising screens, retaining all failures. Keep the incumbent unless a repeatable improvement passes correctness and completion gates.

The load test also needs calibration for its eventual target. A 65,536-transfer admission window permits 200k acknowledgements/s only if mean admission RTT is at most about 328 ms. At 500 ms mean RTT, 200k needs roughly 100,000 outstanding admission credits before extra headroom.

Separately, a 5% surplus of **unique** offered work over 200k canonical TPS for 600 seconds represents 6,000,000 additional transfers awaiting inclusion. The current 2,097,120 global canonical-backlog cap cannot hold that surplus; the source cap of 128 across 24,576 sources provides only 3,145,728 in aggregate. Plan the overload duration, memory budget, per-source limits and drain together. Either use explicit bounded overload intervals plus a steady measurement, or provision and validate sufficient backlog headroom. Do not manufacture excess load by counting retries as new transfers or disable proof/completion checks.

## Priority 1 — controlled parameter calibration

Retain four lanes, the same funded sources, 10 connections, 10 workers, 32 signers, run size 16, batch limit 64 and query cap 64 as the initial reference. The remote runner's `server48` profile overrides imported worker/signer values; use its explicit CLI flags for those treatments.

Run one-factor screens in this order, then repeat only promising changes:

1. Initial/max global admission windows: `32768/65536` versus `65536/131072`, within the existing hard in-flight bound. Observe the actual window and server response, not just the configured ceiling.
2. Fresh-work coalescing: 20 versus 10 versus 5 ms.
3. Physical batch size: 64 versus 128, then 256 only if actual batches fill and available client credit supports them. Retain the original query deadlines and watch admission tail latency.
4. Server `TON_NATIVE_EXECUTOR_THREADS`: 4/8/16, with the actor-scheduler settings fixed. These helpers can create native threads in addition to the actor scheduler; higher values can oversubscribe the host.
5. Transport window: retain 1024 unless producer/queue metrics identify that window as binding; then test 2048 with the logical candidate cap fixed. Queue enlargement alone does not add execution capacity.

Do not begin with hundreds of connections: fixed global credits are divided among them. At the current maximum window, 300 clients have room for only 13 full NTRN parents each; larger batch/query limits cannot override that. Do not increase the 18,432 logical candidate cap before proving that useful packing reaches it and serialized-size headroom remains sufficient.

Parameter calibration may remove an avoidable ceiling. No percentage gain or fourfold outcome is predicted.

## Priority 2 — admission and producer scheduling

This is the first substantive validator change, guided by the new measurements:

1. Cache immutable decoded admission configuration by its exact masterchain state instead of rebuilding it unnecessarily. Measure parsing, state reads, signature verification, actor waiting and final reservation separately.
2. Reduce full client round trips caused by state changes. Evaluate **one bounded refreshed-state validation pass** using the original verified signed parent and original RPC deadline. Recheck current activation/domain, lane/locality, nonce, balance, status and reservation revisions. Do not simply accept a stale snapshot or remove its checks.
3. Give each lane bounded, fair admission work and measure the age of ready producer work. Replace avoidable polling/repeated preparation with prompt queue notifications where the trace demonstrates a delay. Maintain bounded callback work so the pool actor does not monopolize the scheduler.
4. Measure branch-local nonce-floor effectiveness and exclusion scans against the exact parent branch. Preserve the rule that floors never advance the global canonical watermark and never combine competing forks.

Test canonical advances during admission, retries of identical parents, competing parents/nonces, balance changes, expiration, activation/topology changes, cancellation and deadline exhaustion. Promote only when reduced churn leads to higher canonical throughput with identical state/proof results.

## Priority 3 — collation, commit and validation cost

Use profiles after Priority 2, since the dominant cost can move:

- Break native commit into account loading/materialization, staged dictionary updates, checkpoint proof construction, state update/BOC serialization and storage. Earlier component gains are not evidence that another proof rewrite will improve TPS.
- Benchmark persistent bounded executors for signature/state helpers if thread creation or scheduling is material. The current signature helper creates and joins threads per invocation. Keep per-worker reusable scratch and avoid unbounded pools across concurrently active lanes.
- Benchmark staged-trie 1/2/4/8 workers at production distributions and 64/128/256/512 updates before changing the 512 threshold. Account-cell parallelism has a separate 1024-item threshold; do not assume one threshold activates the other stage.
- Parallelize replay only across proven conflict-disjoint account sets or independent lanes. Source-only partitioning is insufficient when transfers share destinations. Preserve deterministic nonce/balance/fee results, root hashes and proofs.
- Revisit checkpoint retention only with measured pending producer work and checkpoint rebuild savings. Keep `TON_NATIVE_CHECKPOINT_RETAIN_INGRESS=0` until a targeted version wins repeated end-to-end tests; the broad experiment regressed previously.

As sizing arithmetic, at 16 aggregate native blocks/s, 200k would require 12,500 logical transfers/block, below the current 18,432 cap. This does not establish that the host can execute, commit, validate and anchor those larger blocks at the same rate. Measure packing and service cost together.

The published workflow already uses Clang 22 Release optimization, native Ed25519 uses optimized OpenSSL, and the validator links jemalloc. Portable builds intentionally avoid assuming the GitHub runner's CPU matches A. Historical desktop cycle images used `PORTABLE=0`/`TON_ARCH=native`, unlike the published portable images; hardware comparisons must account for this build difference. Consider explicit CPU-target variants, ThinLTO and representative PGO after profiling and with a deployment architecture contract. Do not use unrestricted `-march=native` on a hosted runner and distribute it as a generic image. The generator does not currently link jemalloc; test that allocator separately only if generator allocation cost is material.

## Priority 4 — scale independent lanes, then distribute their work

Depth 3 support is implemented across genesis configuration/marker validation, wallet-prefix generation, manifests, readiness, export/import, runner limits and strict eight-lane balance acceptance. The reported eight-lane plateau makes further lane expansion a deferred experiment. First identify the shared admission, workload or validator-processing limit; more lanes on the same host have not demonstrated higher throughput. Depth 4 (16 lanes) still requires coordinated wrapper and validation changes.

Wallet generation needs a deeper-prefix attempt budget or a better generation strategy: a fixed 128 attempts per wallet becomes unreliable across tens of thousands of wallets at depth 4. Use a new isolated genesis and lane-compatible funded accounts for topology experiments; do not treat changing split variables on the current chain as sufficient. Preserve the existing chain and materials.

Use four lanes as the incumbent performance reference and retain the current eight-lane state for diagnosis. Compare matched four/eight-lane results before considering sixteen lanes. Hold total source population fixed for that comparison; separately test holding sources per lane fixed. The latter changes state size and must not be attributed solely to lane count. Do not restart an eight-lane database with depth-2 settings to recover the reference.

Four lanes at 50k imply roughly 12.5k/lane. Maintaining that per-lane rate across 16 independent lanes gives 200k **arithmetically**, or eight lanes at 25k/lane gives the same target. Both are engineering targets, not forecasts: all lanes on A still share its CPUs, pool actor, storage and masterchain work.

Then test actual shard-task placement over 2–4 physical servers if A reaches a measured resource ceiling. Adding validators to the **same shard committee** repeats validation and adds dissemination/quorum work; it does not divide one block's replay. Scaling compute requires distinct shard workloads and appropriately distributed validator groups, with measured assignment coverage and load balance. Retain the desired replication/fault-tolerance level and report it; one validator per shard is not equivalent to a replicated committee. Do not assume four added servers yield a fourfold gain.

Audit shared masterchain, block-serving and database work before claiming horizontal scaling. If proof-serving measurably competes with production, test a dedicated synchronized reader/follower endpoint. Preserve a common proof anchor and verify that the native submission path still reaches the correct state-aware admission owner; adding arbitrary liteservers is not automatically an ingress sharding solution.

## Hardware and network decisions

The exact remote CPU model, physical-core/SMT topology and database device remain unverified. The `server48` name does not establish 48 physical cores.

- **First:** remove demonstrated quota throttling or noisy-neighbor interference; compare NUMA-local CPU/memory placement and adequate headroom for networking/storage. All-core utilization can include retry work, thread churn and stalls.
- **If serial CPU work dominates:** prefer better sustained per-core performance, cache and memory latency. If useful parallel work is already abundant, additional physical cores can help. Select hardware with a replay of this workload rather than core-count marketing.
- **If memory pressure dominates:** improve locality/bandwidth and account-cache behavior. More than 256 GB is not a throughput intervention without capacity pressure, paging or an undersized useful cache.
- **If database I/O dominates:** use local low-latency NVMe and profile state writes/compaction. Do not disable durability to produce a higher score.
- **Network:** current observations around 50 Mbit/s do not identify a saturated 1 Gbit/s link. At roughly 80 serialized block bytes/transfer, 200k corresponds to about 16 MB/s or 128 Mbit/s of block payload alone. This excludes ingress, proofs, replication, framing, retries and bursts; it is not a WAN bandwidth guarantee. Use measured bytes/transfer and latency. Low-latency networking and 10 GbE between additional validator hosts are candidates when replication/transport measurements justify them.

Architecture references: [TON sharding](https://docs.ton.org/foundations/shards), [TON whitepaper, validator task groups](https://ton.org/whitepaper.pdf), and [Linux NUMA locality](https://docs.kernel.org/mm/numa.html). TON's general architecture does not prove that every current fork deployment/configuration already distributes work as desired.

## Delivery order and promotion rule

1. Baseline/measurement and stop/drain reliability.
2. Independent window/coalescing/batch/resource screens.
3. Admission/snapshot and ready-work scheduling improvements.
4. Profile-led state commit and deterministic validation improvements.
5. Recheck four/eight-lane scaling after removing the identified shared limit; defer sixteen-lane support until justified.
6. Hardware comparison and distributed shard placement where the measured ceiling requires them.

Produce an intermediate commit for each isolated change, build both images before its comparison, save effective settings and every result, and keep the best **repeatably validated** version. Confirm the final target with repeated 600-second measurements and then a longer soak, recording proof completeness, final backlog, p95/p99 latency, per-lane balance and resource/network cost. Throughput must not come from omitted validation, unresolved offers, changed transfer counting or hidden state/durability reductions.
