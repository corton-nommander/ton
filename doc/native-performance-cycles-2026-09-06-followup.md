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
