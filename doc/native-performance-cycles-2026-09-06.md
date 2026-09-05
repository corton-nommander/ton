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

The 40k screen passes proof, completion, quantum, ingress, lane, cleanup and strict
continuity. Canonical-capacity holds and measured backpressure are zero; the chain
kept pace with offered load. Native execution cost was 1.59044 microseconds per
accepted candidate transfer. The 50k screen still did not reach 95% of target, with 261,314 full-run
client-capacity holds and zero canonical-capacity holds. Admission headroom needs
another calibration before comparing source changes. Raising
offer and queue headroom is benchmark calibration, not a validator TPS gain.

## Cycle1: source-state reuse

Commit `22c0d00b` keeps the source state loaded by preflight across every output
of its canonical signed run, reserves that source endpoint once, and captures
its rollback snapshot before the first successful write. Full per-transfer state
validation, expired-work handling, size guards and whole-run rollback remain.
The scratch CTest target and all 18 native tests passed before commit. The code
baseline and treatment were built with Release/native Clang 22, four build workers,
and immutable revision labels. Paired live results are pending.

## Next candidate

The production staging population is dominated by small serial updates. A
separate candidate will reuse the existing direct sorted-update constructor with
one worker, restricted to augmentations that explicitly permit reordered pure
construction and plain update cell graphs. The existing root merge remains
serial and threshold 512 stays unchanged. The worker benchmark is extended with
8/16/32-update cases so both old and new executables exercise the dominant sizes.
No performance claim or policy promotion has been made for this candidate.
