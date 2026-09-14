# Native TPS performance test — 14 September 2026

The updated build sustained **56,875 canonical native transfer outputs/second
with the default representation**, averaged across two ten-minute runs. Enabling
the new rooted representation averaged **56,779 TPS**, a **0.17% decrease**.
This experiment does **not establish a TPS improvement**. Keep
`TON_NATIVE_DIRECT_RUN_ZERO_COPY=0`.

| Run order | Rooted flag | Offered / admitted TPS | Canonical TPS |
| --- | ---: | ---: | ---: |
| A1 | 0 | 58,216.48 | 58,688.24 |
| B1 | 1 | 56,742.21 | 57,310.90 |
| B2 | 1 | 55,877.01 | 56,247.56 |
| A2 | 0 | 54,282.64 | 55,061.08 |
| Control mean | 0 | | **56,874.66** |
| Rooted mean | 1 | | **56,779.23** |

The control declined **6.18%** from A1 to A2. The adjacent comparisons disagree:
B1 versus A1 is −2.35%; B2 versus A2 is +2.15%. The mean difference is small
relative to this drift. Four observations on one advancing chain do not establish
statistical significance or isolate the gain from all historical changes.

The earlier [metadata-projection reference](native-master-reference-2026-09-11.md)
really averaged **61,959.20 canonical TPS** across two ten-minute runs. The
current default mean is **8.21% lower**; the rooted mean is **8.36% lower**. This
historical deficit remains unexplained. The latest ABBA isolates the rooted
switch within the current build and cannot exclude regressions in intervening
changes. Metadata projection, overlay signature reuse and admission configuration
caching were enabled in both the historical winner and the current configuration.

The broad workload and CPU allocations matched, but the historical validator used
**14 threads versus 16 now**, reconciliation profiling was on versus off now,
the Docker VM had 64 versus 80 GiB RAM, and the code/images and chain history
differed. Both used named-volume databases with in-memory CellDb and a 16 GiB
cache. None of these differences has been established as the cause. Resolving
the deficit requires the historical and current builds to be compared under the
same thread count, workload and host conditions, starting from equivalent chain
snapshots, before attributing the loss to code or claiming cumulative improvement.

All four runs passed canonical proof, completion, signed-run quantum, batching,
lane-balance, image-reuse and validator-cleanup checks. Together their canonical
measurement windows contain **136,157,360 outputs**. Final backlog, nonce gaps,
hash conflicts, fatal follower errors, retry exhaustion and rejected basechain
candidates were zero. All generators exited successfully. No OOM, unexpected
container restart or resource-guard shutdown occurred.

This is **observed throughput, not demonstrated maximum capacity**. Useful
offered load did not exceed canonical throughput by the required 5% in any arm;
chain-capacity acceptance remained false. Canonical TPS uses the 599 fully
contained integer `gen_utime` seconds within each 600-second offering window.
It can include outputs offered during warmup and differs from offered-cohort TPS.

## What changed in the measured work

Both B runs used rooted parents throughout measured native candidate handling:
accepted validation replayed **69,460,016 outputs from 4,341,251 parents**, with
zero expanded entries. Their collators exercised **49,717 checkpoint groups**.
No checkpoint rollback occurred, so the live rollback branch remains unobserved.
Candidate counts describe work and are separate from canonical throughput.

| Pooled candidate wall time per output | Default | Rooted | Change |
| --- | ---: | ---: | ---: |
| Batch serialization | 0.910 µs | 0.053 µs | −94.15% |
| Strict batch decoding | 1.357 µs | 0.796 µs | −41.36% |
| Broad batch replay | 4.932 µs | 4.437 µs | −10.03% |

These are cumulative stage wall times divided by the corresponding candidate
output counts. They are not CPU costs, and overlapping timers cannot be added as
exclusive attribution. Decoding is outside the broad replay timer; replay includes
signature verification, account loading, execution, materialization and dictionary
checks. The reduction in representation work did not produce a repeatable TPS gain.

## Resource usage and admission retries

Docker statistics filtered to each measured window give:

| Run | Validator mean CPU cores | Validator peak memory | Generator mean CPU cores |
| --- | ---: | ---: | ---: |
| A1 | 9.17 | 14.12 GiB | 0.87 |
| B1 | 8.86 | 17.22 GiB | 0.85 |
| B2 | 9.11 | 18.72 GiB | 0.85 |
| A2 | 9.27 | 20.94 GiB | 0.87 |

Memory increased with run order, including A2 with the feature off; these values
do not isolate a representation memory effect. Across guard samples, the minimum
available memory was **26.30 GiB on the host** and **52.55 GiB in the Docker VM**.
Sampling is approximate, about every two seconds for Docker statistics. Host-PID
based cgroup/thread/device attribution is invalid under this Docker Desktop setup
and is excluded from this report.

| Full-run admission diagnostic | A1 | B1 | B2 | A2 |
| --- | ---: | ---: | ---: | ---: |
| Physical parent submission attempts | 3,169,395 | 3,137,187 | 2,924,201 | 2,856,143 |
| Snapshot-change retry results | 741,584 | 775,322 | 585,437 | 580,514 |
| Canonical-watermark lag results | 49 | 340 | 0 | 0 |
| Timeout results | 82 | 95 | 104 | 103 |

These counters include warmup, measurement and drain, and count physical parent
attempts rather than logical outputs or unique failed transfers. Generator and
validator snapshot-change counts reconcile exactly. Roughly **20–25% of attempts**
encountered a changed admission snapshot and retried; all eventually completed.
This is a concrete overhead to investigate next, but it does not prove a sole
throughput bottleneck. The snapshot-refresh experiment remained disabled.

## Reproduction and remaining limits

The fresh local chain used four lanes, 24,576 source accounts and paired
destinations, 16-output signed runs, batching, ten connections, six workers and
six signers. Each ABBA arm used target-zero load, 60 seconds of warmup, 600 seconds
of measurement and up to 180 seconds to drain. The validator had a 16-CPU quota,
16 threads and a 16 GiB CellDb cache with in-memory CellDb; the generator had six
CPUs and an 8 GiB memory limit. Docker had 24 virtual CPUs and 80 GiB RAM on a
125 GiB host. Unrelated workloads remained running.

Source revision: `925ee0721c5b691debe3a2e37dba83d3daaae581`.
Companion harness: `e77de07689323e2a2439d24ddbd76fe2a878a1e1`.
Fresh images passed 21 publication-test invocations, and the companion reporting
suite passed. A paced control calibration and rooted smoke test passed before
ABBA. Only the rooted flag differed between A and B; other experimental cache,
executor, handoff and scheduler features remained off.

Preparation added Docker `init` for validator services after confirming that the
actual validator lacks a SIGTERM handler. The default-handler probe and actual
run transitions then stopped without forced SIGKILL. This establishes termination
and recovery, not graceful database flushing. The final validator was healthy with
the rooted flag off; the generator and resource guard exited successfully. After
saving the results, the benchmark validator and stats services were stopped to
release resources. Their chain data and artifacts were retained.

All measured images and harness inputs were frozen and hashed. The session-stats
helper is pinned by image ID but lacks a source-revision label, so the harness's
broader reproducibility flag remains false. This local native-output workload
does not establish public-network capacity or general smart-contract TPS.

The [machine-readable result](benchmarks/results/native-rooted-runs-tps-20260914.json)
contains exact image IDs, workload settings, rates, acceptance dimensions,
diagnostics and artifact hashes. Raw logs, proofs, the frozen contract, per-arm
summaries and analysis scripts remain in
`build/benchmarks/native-rooted-runs-tps-925ee0721c5b-20260914T064649Z/`.
