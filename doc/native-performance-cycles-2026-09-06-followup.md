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

The initial proof trial (`301152c7`) passed 27 regressions and all 40,824 screen
checks. Unweighted median proof-cost changes were -38.04% plain and -29.06%
tracked. A 7x7 foreign-usage repeat added 74,088 exact checks; its median was
+0.26%, with two cases above +2% in both pairs. The original anomalous case did
not repeat. All raw cases remain saved in the
[first screen](benchmarks/results/cycles-20260906b-proof-screen-summary.json) and
[fallback repeat](benchmarks/results/cycles-20260906b-proof-fallback-v1-summary.json).

Refinement `13ac90e8` reuses the existing usage-node check to skip RTTI when a
concrete DataCell is already ruled out. It adds no virtual call. The same 27
regressions passed. Its 40,824-check screen improved all 108 plain/tracked cases
in both pairs, with median proof changes -37.72% and -29.64%. Foreign median was
-0.65%, with no case above +2% in both pairs.
[V2 screen](benchmarks/results/cycles-20260906b-proof-v2-screen-summary.json).

The 12,288-base-account tracked follow-up (5 rounds x 5 iterations, A/B/B/A)
passed all 37,800 checks and improved all 54 cases in both pairs. Median proof
wall/CPU cost fell 28.18%/27.95%, and combined copy-plus-proof wall cost fell
28.03%. These are unweighted component-case medians, with construction,
validation and destruction excluded; they are not TPS gains.
[Production-scale component evidence](benchmarks/results/cycles-20260906b-proof-production-summary.json).

## Live proof candidate selection

Prebuild the reference and refined candidate images before the series. Run
A/B/B/A with target 60,000, initial RTT 0.50 s, adaptive cap 65,536, 64 physical
parents/RPC, 20 ms coalescing, 16 logical outputs/parent, 64 queries/client,
finite backlog 2,097,120/source128, four lanes and fixed resource limits.
Use 30 s ramp, 60 s warmup and **300 s measurement** per arm. The longer interval
includes the previously observed pressure episode and gives more time to see
whether it recurs; no samples are discarded and all original gates remain.
Only image switches before setup/warmup may restart the validator. No builds,
unit tests or microbenchmarks may overlap this series. Retention stays off,
parallel threshold stays 512, and no live winner is selected from component data.

Before A1, the control is explicitly finalized as the prebuilt incumbent
`cycle-coalesce-4caa92df`, and the candidate as `cycle-proof-6a96c953`. This
direct comparison selects between incumbent and combined diagnostics/proof code;
it does not attribute live TPS solely to the proof fast path. Both arms use
batching on, so batching defaults are not selected here. The frozen
[series plan](benchmarks/results/cycles-20260906b-proof-live-plan.json) requires
all four capacity gates, matching immutable A/A and B/B image IDs, exact B1/B2
validator continuity and a positive canonical gain in both matched pairs. A
repeatable >=2% claim requires >=2% in each pair. Both images are already built.

## Completed live proof series and retained version

All four 300-second measurements completed, resolved their proof cohorts, and
drained to zero backlog/credits. The fixed image/source/settings/resource/mount
checks passed; B1/B2 kept the same validator process. The raw results are:

| Arm | Version | Offered TPS | Canonical TPS | Target attained | Capacity eligible |
| --- | --- | ---: | ---: | ---: | --- |
| proof-a1 | incumbent | 53,274.56 | 53,152.21 | 88.79% | No |
| proof-b1 | proof + diagnostics | 53,406.51 | 53,314.78 | 89.01% | No |
| proof-b2 | proof + diagnostics | 54,617.33 | 54,781.81 | 91.03% | No |
| proof-a2 | incumbent | 49,136.64 | 49,108.28 | 81.89% | No |

Matched canonical differences are **+0.306%** (A1→B1) and **+11.553%**
(A2→B2), both descriptive and ineligible. Every arm misses the unchanged 95%
ingress gate; B2 additionally offered less than canonical throughput. The
incumbent itself drifted -7.608% from A1 to A2. No repeatable validated TPS gain
was established, and the raw maximum is not promoted. The auxiliary stats
image's missing source revision still leaves reproducibility.valid=false;
immutable runtime continuity nevertheless passed.

The independent [series audit](benchmarks/results/cycles-20260906b-proof-series-audit.json)
verified all hashes, arithmetic, declared settings, resources/mounts and
process identities. Its 72 failed checks are repeated consequences of the
capacity failures: 68 ingress/capacity checks, 3 B2 under-offer checks and 1 series
gate. These are not 72 unrelated faults.
[Pair1](benchmarks/results/cycles-20260906b-proof-live-pair1.json) and
[pair2](benchmarks/results/cycles-20260906b-proof-live-pair2.json) preserve all
metrics and rejected gates.

Measured checkpoint wall cost per accepted candidate transfer fell 13.45% and
25.89% in the two pairs; per-rebuild cost fell 9.36% and 20.29%. Entries per rebuild
increased 4.73%/7.56%, so the per-transfer figures include packing changes. These
are candidate-collation component costs, not unique canonical-transfer costs.
The earlier production-sized offline result remains 28.18% lower proof wall
time across 54 cases, all improved in both repeats, with 37,800 exact checks.

B1's 1,978,048-transfer measured offer deficit reconciles exactly to clipped
pacing credit plus endpoint balance change. Client-credit holds occupy 29.87%
of measured worker time. Its admission window fell from about 31,903 to 7,582
around 41 timeout parent outcomes; the 65,536 cap never bound. A2 has 579 timeout
parent outcomes versus A1's 48, with 324 re-signs and 255 timeout retries. These
pressure differences prevent attributing the larger pair 2 TPS difference to
proof traversal. Full-run retry/admission counts are explicitly separate from
measured-phase pacing and collation costs.

The trial is preserved in local branch `perf/proof-trial-20260906` and immutable
image `cycle-proof-6a96c953`. Production code on master is restored to the
incumbent `4caa92df` implementation, while the results and trial commits remain
in history. The incumbent validator is retained, batching defaults off,
checkpoint retention stays off and the worker threshold stays 512.
[Selection ledger](benchmarks/results/cycles-20260906b-winner.json).

The next bounded change should distinguish admission-deadline expiry from
signed-message validity expiry. Source review confirms the existing broad
`expired` match can take an admission timeout into real re-signing, clearing
serialized state/resetting attempts instead of scheduling the timeout retry.
The behavior also exists in the incumbent. Preserve timeout congestion
handling, retry horizon, hashes and actual signed-expiry recovery; add precise
timeout-origin/window-decrease attribution before changing policy. Saved counts
do not identify each timeout's origin or prove this fix will increase TPS.
[Diagnosis and proposed coverage](benchmarks/results/cycles-20260906b-admission-deadline-expiry-source-review.json).

The four independent measured-window audits retain exact row/count/stage
reconciliation and packing/timeout context:
[A1](benchmarks/results/cycles-20260906b-proof-a1-independent-analysis.json),
[B1](benchmarks/results/cycles-20260906b-proof-b1-independent-analysis.json),
[B2](benchmarks/results/cycles-20260906b-proof-b2-independent-analysis.json), and
[A2 plus paired analysis](benchmarks/results/cycles-20260906b-proof-a2-independent-analysis.json).
Frozen benchmark sources and audit tools are archived in
[the reproduction bundle](benchmarks/tools/proof-accounting-20260906/README.md).

Final verification: source restoration commit `e1c37e8c` has an empty
non-document delta against `4caa92df`; preserved trial branch
`perf/proof-trial-20260906` points to `0a298195` and has an empty non-document
delta against measured candidate `6a96c953`. Both repositories were clean.
The incumbent image is healthy, the load generator is stopped, and harness
batching/retention defaults remain 0 with staged-worker threshold 512.
[Verified retained state](benchmarks/results/cycles-20260906b-retained-state.json).
