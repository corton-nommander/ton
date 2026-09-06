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

The strict diagnostic pair completed with matched source labels, resource limits,
container settings and harness revision. A1 offered 41,177.60 / canonical
42,509.29 TPS during a large transient (maximum RTT 9.93 s); B1 offered
55,020.00 / canonical 55,041.90 TPS. Both fail capacity comparison: target
attainment is 71.00% and 94.862%, and both are under-offered. The +29.48%
descriptive canonical difference is **not an instrumentation speedup**. The
[comparison](benchmarks/results/cycles-20260906b-telemetry-screen.json) preserves
these invalid results rather than promoting the largest number.

B1 telemetry is complete. Its 178,800-transfer offered deficit reconciles exactly
(to <0.000001 transfer) as 178,522.128514 discarded tokens plus 277.871486
additional endpoint token balance. Observed client-capacity waits total 58.414
worker-seconds out of 360 (16.23%); maximum measured pump gap is 52.967 ms and
maximum pump duration 7.602 ms. The effective window never reaches its 24,576
cap (sampled maximum 16,599; initial 11,600).
[Pacing evidence](benchmarks/results/cycles-20260906b-pacing-diagnostic-analysis.json).

The next calibration increases the initial RTT budget to 0.50 s and maximum
window to 65,536 at target 60,000, preserving the 100 ms pacing cap and all finite
backlog, proof, cleanup and strict-reuse gates. This distinguishes insufficient
client headroom from a pacing-policy change before adding another knob.

The larger-initial-window calibration offered 56,242.40 / canonical 55,752.14
TPS. Offered margin was +0.879%, but target attainment was only 93.737%, so it
remains rejected. All evidence is saved in
[window calibration](benchmarks/results/cycles-20260906b-window64k-calibration.json).
Neither target attainment nor capacity gates were relaxed.

All 41,945 B1 code-651 outcomes are exact snapshot-change rejections; mean batch
residence is 64.830 ms, with 56.019 ms in verification dispatch/wait. This is wall
time, not signature CPU. The avoidable fraction remains unknown: snapshot
identity changes alone do not prove source/config equivalence.
[Admission evidence](benchmarks/results/cycles-20260906b-admission-diagnostic-analysis.json).

## Cycle 7: proof-accounting traversal

Production telemetry has 2,715 staged calls, all one worker, with 52.19% at
129–256 updates and 29.91% at 33–64. None reaches 257. Checkpoint rebuild costs
4.910855 s (34.74% of native commit); staged dictionary work costs 6.716273 s.
The public manifest confirms 12,288 unique native endpoints per lane, which is
not a claim about total ShardAccounts cardinality.
[Population evidence](benchmarks/results/cycles-20260906b-trie-population-analysis.json).

The isolated candidate keeps all reference counts, deduplication, usage-tree
cutoffs, parent-set accounting and child virtualization. For concrete ordinary
nonvirtualized level-zero DataCells it reads bits/references directly; other
cells keep the existing NoVm CellSlice path. Differential regressions compare
against frozen pre-change traversal, including wrapper callbacks and failures.

Initial offline screening is predeclared as A/B/B/A, 3 rounds x 3 iterations,
4,096 base accounts, all three plain/tracked/foreign-usage modes, uniform and
prefix2 keys, 8/16/32/64/80/128/192/256/512 updates and 1/4/16 exact checkpoints.
That is 40,824 timed checkpoint comparisons, plus warmups. The reference binary
is preserved from before applying the candidate; driver/oracle hashes must
match both binaries. No builds or live load may overlap timed runs. These are
component measurements and cannot establish a TPS gain.
