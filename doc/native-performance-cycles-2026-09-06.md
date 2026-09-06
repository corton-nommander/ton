# Native performance cycles, 2026-09-06

## Baseline and protocol

The user committed recovery as `ff3e0493`. Commit `4a54b12b` preserves its raw
benchmark evidence and is the first image baseline. Companion harness commit
`b87012f` records the previously validated strict-image and capacity safeguards.
All source changes and results below are local commits; images are prebuilt
locally once per revision before measurement. No registry or CI publication is
part of these local cycles.

The existing depth-2 chain and its 24,576 sources are retained. Four-lane profile,
16-transfer normal signed-run quantum, validator/generator CPU allocations,
retention off, and trie worker threshold 512 remain fixed. Screens use 30 seconds
of ramp, 60 seconds of warm-up, 60 seconds of measurement and 180 seconds of drain.
Every run requires strict prebuilt reuse; no builds run during load. Correctness,
completion, quantum, ingress, lane and cleanup checks are unchanged. Actual
offered TPS must exceed canonical chain-window TPS for any capacity claim.

Run commands and full local logs are in `build/benchmarks/cycles-20260906/`.
Full raw bundles are in sibling `MyLocalTonDocker/benchmark-results/` with prefix
`cycles-20260906-`. Compact tracked JSON extracts preserve raw-summary hashes,
image identities, acceptance reasons, normalized candidate costs, and counter
windows. Session-stats lacks a revision label, so its immutable image ID is
recorded but the harness independently retains `reproducible=false`.

## Offered-load calibration

The previous 39k diagnostics were secretly blocked by a canonical backlog guard.
A limit of 131,072 divided across six workers leaves each at 21,840 after whole
16-transfer runs, below its configured 21,845/21,846 limit. The old pause predicate
compared backlog to the limit and missed this unusable residual. At the recorded
control plateau, aggregate backlog was exactly 131,040, with zero inflight/ready/
signing work and 3,900 unused pacing tokens. Canonical-capacity issue holds were
11,827/8,303 while reported pause time was zero.

Commit `c81d9589` makes backpressure accounting use the actual issuance quantum.
The policy suite passes residual, resume, scalar, disabled, drain and uint64
boundary regressions. It does not weaken the 1% backpressure or other acceptance
thresholds. The first image pair predates this telemetry correction, so it uses
a quantum-aligned global cap of 2,097,120 (349,520 per worker) and source cap 128;
canonical-capacity issue holds are checked in addition to timed backpressure.
These finite caps apply equally to every image in a pair.

| Screen | Requested TPS | Admission window | Offered TPS | Canonical TPS | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| [baseline40k](benchmarks/results/cycles-20260906-source-a1-40k-60s.json) | 40,000 | 768 | 38,767.47 | 38,770.17 | Load validation; no capacity claim |
| [baseline50k](benchmarks/results/cycles-20260906-source-a1-50k-60s.json) | 50,000 | 1,536 | 46,407.20 | 46,571.39 | Rejected: target not attained; offered below canonical |
| [baseline50k-window3072](benchmarks/results/cycles-20260906-source-a1-50k-cwnd3072-60s.json) | 50,000 | 3,072 | 48,297.07 | 48,213.69 | Capacity gates pass; thin offered margin |

The 40k screen passes proof, completion, quantum, ingress, lane, cleanup and strict
continuity. Canonical-capacity holds and measured backpressure are zero; the chain
kept pace with offered load. Native execution cost was 1.59044 microseconds per
accepted candidate transfer. The 50k screen still did not reach 95% of target, with 261,314 full-run
client-capacity holds and zero canonical-capacity holds. Admission headroom needs
another calibration before comparing source changes. With window 3072, all
required gates pass, canonical-capacity holds remain zero and execution costs
1.43207 microseconds per accepted candidate transfer. Offered over canonical is
only 1.00173: this is a short qualified screen, not a maximum-capacity conclusion.
The source treatment uses exactly this same target, window and backlog profile. Raising
offer and queue headroom is benchmark calibration, not a validator TPS gain.

## Cycle 1: source-state reuse

Commit `22c0d00b` keeps the source state loaded by preflight across every output
of its canonical signed run, reserves that source endpoint once, and captures
its rollback snapshot before the first successful write. Full per-transfer state
validation, expired-work handling, size guards and whole-run rollback remain.
The scratch CTest target and all 18 native tests passed before commit. The code
baseline and treatment were built with Release/native Clang 22, four build workers,
and immutable revision labels. The first matched live screen is complete:

| Metric | Baseline | Source reuse |
| --- | ---: | ---: |
| Offered TPS | 48,297.07 | 48,749.07 |
| Canonical TPS | 48,213.69 | 48,871.59 |
| Native execution microseconds / accepted | 1.43207 | 1.41423 |
| Classification | Capacity gates pass | Load validation |

[Paired evidence](benchmarks/results/cycles-20260906-source-first-pair.json)
confirms identical launch settings and harness environment hashes. All proof,
completion, ingress, quantum, lane, cleanup and strict-continuity checks pass.
Execution cost is descriptively 1.25% lower and TPS 1.36% higher. Treatment offered
load is below canonical throughput; no capacity gain or repeatability claim is
made. The >=15% execution target is not met by this screen. The change is retained
for its removed redundant work while further cycles address the larger trie cost
and insufficient admission headroom.

## Next candidate

The production staging population is dominated by small serial updates. A
separate candidate will reuse the existing direct sorted-update constructor with
one worker, restricted to augmentations that explicitly permit reordered pure
construction and plain update cell graphs. The existing root merge remains
serial and threshold 512 stays unchanged. The worker benchmark is extended with
8/16/32-update cases so both old and new executables exercise the dominant sizes.
No performance claim or policy promotion has been made for this candidate.


## Serial trie candidate: offline A/B/B/A

The guarded serial direct constructor passed all seven dictionary tests and
18 native state tests. Four complete matrices ran in A/B/B/A order using saved
baseline/candidate executables with identical deterministic fixtures. Each
contained 240 cases, five rounds and five iterations per CSV row. The 4,800 rows
therefore represent **24,000 timed root comparisons**, plus 1,920 warmup root
comparisons; 960 first warmups also passed full dictionary validation. Every
process exited successfully. Genesis was paused during this offline work and
resumed afterward; no build or live load overlapped.

[One-worker results for all sizes/topologies](benchmarks/results/cycles-20260906-trie-serial-offline-summary.csv)
and [binary hashes, raw CSV hashes and run provenance](benchmarks/results/cycles-20260906-trie-serial-offline-provenance.json)
preserve the comparison. Baseline/candidate matrix elapsed times were
14.034/13.075/12.976/14.006 seconds, including setup and all worker tiers.
Per-operation comparisons below use only the timed dictionary stage. Pair 1
compares A1/B1 and pair 2 compares A2/B2 at the same round and fixture. Positive
percentages mean less time in the candidate.

| Updates | Depth2 tracked wall reduction, pair 1 | Depth2 tracked wall reduction, pair 2 | Depth2 tracked CPU reduction, pooled | Depth2 plain wall reduction, pooled |
| --- | ---: | ---: | ---: | ---: |
| 8 | 8.5% | 14.9% | 11.8% | 13.6% |
| 16 | 12.4% | 15.1% | 14.4% | 16.7% |
| 32 | 13.2% | 15.4% | 14.4% | 16.6% |
| 64 | 18.3% | 19.6% | 18.4% | 20.8% |
| 80 | 19.6% | 22.1% | 21.0% | 25.4% |
| 128 | 23.6% | 23.6% | 23.5% | 27.0% |
| 192 | 27.0% | 27.3% | 27.0% | 30.5% |
| 256 | 31.5% | 31.0% | 31.0% | 34.2% |
| 384 | 34.3% | 34.3% | 34.3% | 35.3% |
| 512 | 36.5% | 38.1% | 37.3% | 38.0% |

All 60 one-worker size/distribution/prior-state configurations improved in
pooled paired wall-time medians; 59 improved in both repetitions. The dominant
production population is at most 64 updates, with mean staged set 34.14. At 32
and 64 updates on tracked depth2 state, both repetitions reduced wall time by
13.2–15.4% and 18.3–19.6%, respectively, with corresponding CPU reductions.
The <=64 histogram does not establish exact 8/16/32 frequencies, so no weighted
production gain is inferred.

One result remains inconsistent: plain depth2 at 32 updates improved 17.6% in
pair 1 but regressed 12.4% in pair 2. During that B2 sample block the unchanged
2/4/8-worker paths also slowed, which suggests interference but does not prove
its cause. Across the complete matrices those unchanged tiers had median
A/B speedups of 1.010/1.002/1.003. The outlier remains in the preserved results
and should be checked again after the live measurement window.

The result is sufficient to justify a prebuilt live TPS experiment. It does
not establish a validator TPS gain or change worker policy: the production
parallel threshold remains 512, and all production checkpoint attempts observed
in the recovery screen selected one worker. The candidate only changes private
update-trie construction under existing augmentation and plain-cell guards;
tracked prior-state merging remains serial.


## Cycle 2: live serial trie screen

Both images were prebuilt before the pair. Control uses source-reuse image
`22c0d00b`; treatment image `4c200e69` includes serial direct construction
`7e51f921` and the quantum backpressure-accounting correction `c81d9589`.
The latter has no observed effect under the shared quantum-aligned caps: both
runs report zero canonical-capacity holds. Target 55000, admission window 6144,
backlog 2097120/source 128, four lanes and all other launch settings are identical.

| Metric | Control | Trie treatment | Descriptive change |
| --- | ---: | ---: | ---: |
| Offered TPS | 52,632.80 | 51,406.13 | -2.33% |
| Canonical TPS | 52,580.34 | 51,361.36 | -2.32% |
| Execute microseconds / accepted | 1.46405 | 1.32094 | -9.77% |
| Commit microseconds / accepted | 6.08695 | 4.80691 | -21.03% |
| Staged trie microseconds / accepted | 2.77707 | 2.02784 | -26.98% |
| Storage-proof rebuild microseconds / accepted | 2.51952 | 2.01579 | -19.99% |

[Paired evidence](benchmarks/results/cycles-20260906-trie-first-pair.json) retains
the classification and normalized stage arithmetic. Control passes capacity
gates with a thin offered margin; treatment fails 95% target attainment. Proof,
completion, quantum, lanes, cleanup and strict continuity pass. Stage timings
remain descriptive because batching/population and scheduling can change under
these offered rates; the unchanged execution/proof stages also moved. Independent
offline repetitions support the algorithm's serial-trie improvement. There is
no validated TPS gain from this pair, and no repeated capacity pair is claimed.
The tested trie optimization is retained while the next cycle addresses the
transport admission ceiling using an explicit default-off batching mode.


## Cycle 3: explicit intact-parent transport batching

Commit `d5fc1b9a` adds `--native-run-batching`, default off. It immediately
groups already-ready whole NTRN parents from distinct source heads, falling
back to individual submission for one parent. Physical message/byte bounds,
worker/client logical credits, per-parent retry ownership, and per-output AIMD
acknowledgment remain separate. It adds no coalescing delay or signed-run padding.
The three policy, query-routing and admission CTest targets pass.

Harness commit `559a519` wires an explicit `NATIVE_LOAD_NATIVE_RUN_BATCHING`
boolean, checks the image's exact flag support, and validates requested/effective
telemetry. Its full reporting/configuration regressions and shell/Compose checks
pass. Existing admission, quantum, proof, cleanup, strict-reuse and capacity gates
are unchanged. [Validation evidence](benchmarks/results/cycles-20260906-intact-batch-validation.json)
records the tested source and harness revisions.

The candidate image is built once before live integration. Subsequent off/on
runs will reuse that same image and validator process; offered-rate calibration
remains necessary. Unit tests alone do not validate the actor/RPC lifecycle or
establish a throughput gain.


The initial enabled-mode integration screen used target 55,000, window 12,288,
initial RTT 0.20 s and64 queries/client. It delivered53,694.4 offered TPS and
53,410.44 canonical TPS, passed all capacity/correctness/quantum/lane/cleanup
and strict-continuity gates, and drained to zero logical/query inflight. Its
0.532% offered margin remains thin evidence of saturation. Parent attempts,
logical attempts and query identity reconcile exactly; all attempts carried16
outputs. Only 1.196% of parents used transport batches (947 queries, mean 5.86
parents), so the immediate collector reduces total queries by only 0.992%.
[Diagnostic evidence](benchmarks/results/cycles-20260906-batch-diagnostic-55k-cwnd12288.json)
preserves all counters. This is integration validation, not an A/B improvement.

The next same-image screen fixes target 60,000 with the same 12,288 window,
0.20 s initial RTT,64 queries/client, finite backlog caps and phase durations.
Planned order is off/on/on/off; a failed first capacity pair is retained and
diagnosed before repetition. Both modes must attain at least 57,000 offered TPS
and offered must exceed canonical; a 5% margin would give stronger saturation
evidence. No restart or rebuild occurs between arms.


The 60k same-image off/on screen is complete:

| Metric | Off | Immediate batching |
| --- | ---: | ---: |
| Offered TPS | 54001.07 | 53498.67 |
| Canonical TPS | 53975.32 | 54319.46 |
| Execute microseconds / accepted | 1.32811 | 1.32182 |
| Capacity comparison eligible | No | No |

Both arms fail 95% target attainment; treatment also offers less than canonical
production. The descriptive +0.64% canonical change cannot support a gain claim.
Only the intended mode flag changed, and validator container/daemon identity and
both images are identical across arms. Proof, run completion, 16-output quantum,
per-parent/query accounting, lanes, strict reuse and cleanup pass. The enabled
arm batches only 1.235% of parents. [Paired evidence](benchmarks/results/cycles-20260906-batch-first-pair.json)
retains both rejected runs. As predeclared, the ineligible pair is not repeated.

This weak grouping motivates the next bounded experiment: use the existing
absolute coalescer deadline only for fresh parents in explicit batching mode.
Retries, repairs and drain remain prompt, and parent quanta remain indivisible.
The candidate must pass ownership/deadline/credit regressions before a new image
and live integration run. Immediate batching remains default off.


## Cycle 4: bounded fresh-parent coalescing

Commit `4caa92df` applies the existing absolute coalescer only to explicit native
batching. Fresh source heads wait at most the configured 20 ms for transport
grouping, or release early when actual physical/logical credit is full. Deadline
expiry survives credit stalls. Retries, repairs and drain bypass the fresh gate.
Held parents requeue intact; reset checks inspect ready tokens, without scanning
all wallets. Released singletons avoid batch scratch allocations. The three
policy/routing/admission CTest targets pass; [validation evidence](benchmarks/results/cycles-20260906-coalescer-validation.json)
records remaining actor integration limits. Harness documentation commit
`0cc494e3` describes the opt-in behavior without changing any values or gates.

The candidate image is prebuilt once. Its first integration diagnostic uses
target 55,000, window 12,288, initial RTT 0.20 s,64 queries/client and 20 ms coalescing,
with the existing finite backlog caps and 60 s measured window. Transport batch
density, query reduction and measured-window generator CPU determine whether
this experiment warrants longer paired repetitions. Repeated ready probing can
add allocation/queue overhead; no performance gain is assumed.


The coalescer integration passes all gates: 53,541.6 offered TPS, 53,486.37
canonical TPS, zero outstanding logical/query credits and canonical backlog
after drain. Exactly 16 logical outputs per parent attempt and ordered query
accounting reconcile. It batches 99.801% of parents, averaging 11.387 per batch;
43,990 RPCs represent 490,788 parent attempts, a 91.037% reduction versus one RPC
per parent. Measured generator CPU averages 0.734 cores, validator 8.652 cores.
The offered margin is only 0.103%, so this single screen does not prove maximum
capacity or a TPS improvement. [Diagnostic evidence](benchmarks/results/cycles-20260906-coalesce-diagnostic-55k-cwnd12288.json)
retains all counters and gates.

The predeclared [180-second off/on/on/off protocol](benchmarks/results/cycles-20260906-coalescer-abba-plan.json)
ran at the same 55k target/window/query limits. No images are rebuilt
or validator restarted between arms. Every arm is preserved; capacity gain and
repeatability require both eligible comparisons, not a selected best run.


The first 180-second pair is saved. Control offers 51,907.64 TPS and produces
52,291.49 canonical TPS; treatment offers 53,817.69 and produces53,877.99.
The descriptive canonical difference is +3.03%, with native execution cost
1.38769 to 1.31576 microseconds/accepted (-5.18%). Control fails target attainment
and both arms offer below their canonical window rates, so the pair cannot
support a capacity improvement. Proof, run completion, quantum, transport-credit
accounting, lane, cleanup and strict identity checks pass.
[First long pair](benchmarks/results/cycles-20260906-coalescer-long-pair1.json)
is retained before the reversed-order repetition; no best-run selection occurs.


## Completed 180-second coalescer A/B/B/A series

All four arms used identical prebuilt validator and generator images and the
same validator container, daemon PID and kernel start ticks. Only the batching
flag changed. Proof completion, 16-output quantum, lane balance, query/logical
credit reconciliation, strict continuity and final cleanup pass in every arm.

| Metric | Off A1 | On B1 | On B2 | Off A2 |
| --- | ---: | ---: | ---: | ---: |
| Offered TPS | 51,907.64 | 53,817.69 | 52,638.58 | 52,090.93 |
| Canonical TPS | 52,291.49 | 53,877.99 | 51,638.97 | 51,035.53 |
| Execute microseconds / accepted | 1.38769 | 1.31576 | 1.29674 | 1.43572 |
| Generator mean CPU cores | 0.7833 | 0.7257 | 0.7131 | 0.7674 |
| Queries saved per parent attempt | 0% | 91.36% | 91.29% | 0% |
| Not-ready retry schedules / parent attempts | 1.40% | 7.78% | 7.82% | 1.30% |
| Capacity classification | Rejected | Load validation | Eligible | Rejected |

Both controls fail 95% offered target attainment. A1 and B1 also offer less
than canonical production. Only B2 is individually capacity-eligible. The
[complete aggregate](benchmarks/results/cycles-20260906-coalescer-long-abba.json)
preserves all arms, both pairs, input hashes and eligibility decisions.

Descriptive TPS changes are +3.03% and +1.18%; native execution cost changes
are -5.18% and -9.68%. Neither pair supports a capacity gain, and the >=2% TPS
or >=15% execution targets are not established. Generator sampled CPU per
offered transfer falls 10.64% and 8.04%. Validator sampled CPU per canonical
transfer changes +0.01% and +0.34%, so no overall validator CPU saving is claimed.
CPU figures use Docker samples within the measured window, with interval and
workload limitations; native execution timings use accepted candidate transfers.

The transport grouping effect repeats: over 99.8% of parents are batched,
averaging 11.77 and 11.68 parents per batch. The higher not-ready retry count
is a material tradeoff. These are full-run retry schedules per physical attempt,
not distinct failed transfers. Every offer eventually resolves by canonical
proof. Overlapping admission snapshots are a possible explanation for the
additional retries; typed counters alone do not establish the cause. Batching
remains explicitly default-off.

## Retained changes and remaining targets

Four improvement/commit/test/evidence cycles are complete. Source-state reuse
and serial sorted-update trie construction remain enabled. Independent trie
microbenchmarks repeated 13.2–15.4% and 18.3–19.6% wall-time reductions at
32 and 64 tracked depth-2 updates. The load generator has tested opt-in intact
parent batching and bounded coalescing, with enforced requested/effective mode
telemetry. The trie parallel threshold remains 512 and ingress checkpoint
retention remains off.

No repeatable eligible TPS gain is claimed. The original nonce-floor and
heap-free-scratch changes were already in the user's committed baseline and
are not isolated by these new cycles; their original numerical targets remain
unvalidated here. All failed runs are retained. The session-stats image still
lacks a source-revision label, so source reproducibility remains reported false
even though strict image/process reuse passes.

The remaining admission/pacing limit needs lost-token and held-time evidence
before another capacity calibration. The [pacing audit](benchmarks/results/cycles-20260906-pacing-audit.md)
finds no cohort or quantum-rounding error. A 100 ms token bucket silently clips
excess credit, and admission latency tails exceed that interval; existing event
counts cannot quantify clipping or time blocked. Targets and gates were not
relaxed to compensate.


Final local runtime is healthy on prebuilt image `cycle-coalesce-4caa92df`;
the generator is stopped and no measurement remains running.
[Final state](benchmarks/results/cycles-20260906-final-state.json) records code
commits, harness revision, defaults and unproven targets. The final independent
audit verified all four raw-summary hashes, actual environments, identities,
proof cleanup, counter arithmetic, retry ratios and rejected capacity decisions.


## Native client APIs and 10/50/100 persistent connection sweep

Native tonlib address/state, signed message construction, exact hashes and ordered
batch submission are implemented and live-verified. A later fresh-network sweep
used persistent ADNL/TCP submissions against one unchanged validator/liteserver,
with 60 seconds warm-up and 180 seconds measurement per arm.

| Connections | Offered/admitted logical TPS | Canonical logical TPS | Capacity result |
| ---: | ---: | ---: | --- |
| 10 | 62,528.09 | 62,800.80 | Observation only |
| 50 | 62,955.73 | 63,433.12 | Observation only |
| 100 | 63,214.49 | 60,250.28 | Eligible |

All arms passed proof, completion, 16-transfer signed-parent density, lane,
batching, ingress, cleanup and strict image/process reuse. All drained to zero.
Only 100 clients supplied sufficient offered overdrive (4.92%) for chain-capacity
acceptance. The descriptive 50-versus-10 gain is 1.01%; 100 versus 50 is -5.02%.
No repeatable gain or historical code-level uplift is claimed: the old containers
and volumes were absent, so this used a freshly provisioned four-lane chain.

[Full client sweep report](native-client-connections-2026-09-06.md) and
[machine-readable results](benchmarks/results/native-client-connections-20260906.json)
save the commits, all classifications, raw hashes, exact images, live API test,
and both the highest observed (50) and capacity-qualified (100) configurations.
The later active validator image is `cycle-clients-ed666c9a`, with the corrected
prebuilt generator `cycle-clients-ed666c9a-h2`; the validator remained unchanged
throughout the completed series and the generator is stopped. Session Stats still
lacks a source-revision label, preserving the separate reproducibility limitation.
