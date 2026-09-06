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

Initial calibration changes physical parents per RPC from64 to16 at a fixed20ms
coalescer, retaining16 logical outputs per signed parent. Both arms reuse the
previous prebuilt image and validator process. Target55000, window12288, initial
RTT0.20s, query limit64, finite backlog2097120/source128, phase durations
30/60/60/180s and four lanes are fixed. This compares configuration, not new
validator code. Source-only measurement instrumentation is prepared concurrently;
no compiler, unit tests or microbenchmarks run during live load.

The [saved plan](benchmarks/results/cycles-20260906b-plan.json) records exact
selection rules and calibration settings.
