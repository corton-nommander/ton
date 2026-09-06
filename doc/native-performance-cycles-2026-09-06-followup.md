# Native performance follow-up, 2026-09-06

The previous cycle ended at `3e40ddc0`, with source image `cycle-coalesce-4caa92df`
and batching default off. The user requested further measured cycles, intermediate
commits, saved results and retention of the highest-TPS version.

## Selection protocol

Promote a candidate only after matched repeated measurements pass all existing
correctness, completion, quantum, ingress, lane, cleanup and strict-reuse gates,
with actual offered load above canonical throughput for capacity claims. Preserve
the incumbent when no candidate qualifies; do not select the largest rejected
number. Code, generator settings, image IDs and failed runs remain in the ledger.

Initial calibration changes physical parents per RPC from 64 to 16 at a fixed 20 ms
coalescer, retaining 16 logical outputs per signed parent. Both arms reuse the
previous prebuilt image and validator process. Target 55,000, window 12,288, initial
RTT 0.20 s, query limit 64, finite backlog 2,097,120/source 128, phase durations
30/60/60/180 s and four lanes are fixed. This compares configuration, not new
validator code. Source-only measurement instrumentation is prepared concurrently;
no compiler, unit tests or microbenchmarks run during live load.

The [saved plan](benchmarks/results/cycles-20260906b-plan.json) records exact
selection rules and calibration settings.

## Cycle 5: physical batch-size screen

Both arms passed strict image/process continuity, transport accounting, proof and
cleanup checks. The 16-parent candidate is rejected; it failed the existing
95% ingress-target gate. The control offered slightly less than canonical TPS,
so this screen supports no capacity gain claim.

| Physical parents/RPC | Offered TPS | Canonical TPS | Execution us/accepted | not_ready schedules/parent attempt | Decision |
| --- | ---: | ---: | ---: | ---: | --- |
| 64 | 53,572.00 | 53,649.63 | 1.29158 | 8.10% | Under-offered; retain as reference configuration |
| 16 | 50,587.73 | 50,266.31 | 1.36569 | 9.67% | Reject; ingress target missed |

The descriptive canonical difference is -6.31%; execution cost is +5.74%.
These are screening observations, not a validated regression estimate. Source,
images and validator process were identical, and actual container settings
changed only the declared physical batch size. [Full comparison](benchmarks/results/cycles-20260906b-batch-size-screen.json).

A separate load calibration now tests target 60,000 with aggregate window 24,576
and 64-parent batches on the same prebuilt image. It changes load parameters to
seek sufficient offered load and cannot be counted as a code speedup.

The larger-window calibration offered 55,596.80 TPS and produced 54,884.88
canonical TPS (1.30% offered margin). It still failed the unchanged 95% ingress
gate: 92.66% of its 60,000 target. Proof, cleanup, strict continuity and normal
16-output density passed. This result is retained as load calibration, with no
code-speedup or capacity promotion. [Saved evidence](benchmarks/results/cycles-20260906b-window24k-calibration.json).

## Cycle 6: locate client and admission delays

The next image adds measurement only: pacing credit discarded by the existing
bucket cap, pump/update/alarm timing maxima, measured client-capacity wait
worker-seconds, asynchronous worker timestamps, final admission retry causes and
batch residence timings. It also splits the existing <=64 account histograms
into <=8, 9–16, 17–32 and 33–64. No pacing, retry, checkpoint or worker policy
changes belong to this image.

Metric scopes are documented in [pacing diagnostics](native-load-pacing-diagnostics.md)
and [admission diagnostics](native-admission-diagnostics.md). Harness commit
`096aea7` captures additive full-run admission deltas and measured fine trie
populations. Its reporting regressions and shell syntax checks passed; missing
old-image counters remain explicitly unavailable. Source build and focused C++
regressions precede image preparation and the next measurement.

Instrumentation validation completed: the generator and validator built, and all
four focused CTest suites passed (pacing policy, query accounting, native batch
admission, and collation-stat serialization). Intermediate source commits are
`379f593c`, `185002f3`, and `19e32eea`. An independent
[188-check audit](benchmarks/results/cycles-20260906b-saved-results-audit.json)
found no material mismatch in the three saved runs or winner decision.

The first diagnostic measurement uses target 58,000, window 24,576, 64 physical
parents per RPC and 20 ms coalescing. This is a declared load calibration; only
subsequent matched runs at fixed settings can establish a code improvement.
