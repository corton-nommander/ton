# Native load pacing diagnostics

This document describes the preserved, unpromoted diagnostic trial in local
branch `perf/proof-trial-20260906` (live image source `6a96c953`). Production
code on master retains incumbent `4caa92df` after the full live series failed
capacity gates. See [the selection report](native-performance-cycles-2026-09-06-followup.md).

The generator emits `pacing_telemetry` in every JSON report and repeats the
object in `worker_pacing_snapshots[]`. These are diagnostic observations. They
do not change the target, token bucket, timers, issue gates, or benchmark
acceptance criteria.

The measured phase is the configured half-open interval
`[run_start + ramp + warmup, run_start + ramp + warmup + duration)`, using the
same monotonic clock as issuance. Measured telemetry uses that interval rather
than differences between periodic reports. The table describes the aggregate
object; each worker object uses the same fields for that worker alone.

| Fields | Meaning and aggregation |
| --- | --- |
| `clipped_tokens`, `measure_clipped_tokens` | Sum of logical pacing credit discarded by the existing bucket cap. The all-run value includes refill during drain; the measured value includes only overflow attributed to the measured phase. |
| `clipped_updates`, `measure_clipped_updates` | Sum of refill updates with positive discarded credit in the corresponding scope. These count updates, not transfers or distinct stalls. |
| `token_update_gap_max_s`, `pump_gap_max_s`, `pump_duration_max_s`, `alarm_lateness_max_s` | Maximum across workers of a complete observed interval: successive paced refills, successive pump starts, one pump invocation, or scheduled alarm deadline to callback entry. |
| Corresponding `measure_*_max_s` timing fields | Maximum across workers of the portion of each observed interval intersecting the measured phase. A long interval straddling the phase is clipped to its overlap, rather than included in full. |
| `measure_start_samples`, `measure_end_samples` | Number of workers that observed a refill interval containing the exact boundary. Each boundary contributes at most once per worker. |
| `measure_start_refilled_tokens`, `measure_end_refilled_tokens` | Sum of inferred refilled balances at the exact boundary, before the next issue turn. These are not direct snapshots of the stored bucket and are not the final post-drain token gauge. |
| `measure_start_max_service_lag_s`, `measure_end_max_service_lag_s` | Maximum delay from the exact boundary until the token update that first observed it. |
| `native_client_issue_wait_worker_s`, `measure_native_client_issue_wait_worker_s` | Sum of worker time in an observed whole-quantum NTRN client-capacity hold, for the issue period or its measured overlap. These are worker-seconds, not the wall-time union of simultaneous holds. |
| `native_client_issue_wait_events`, `measure_native_client_issue_wait_events` | Sum of observed hold intervals; measured events require positive measured overlap. Repeated pumps or report snapshots do not restart or count an open interval again. |

Clipped credit uses the unchanged endpoint-trapezoid refill model. Within an
update interval, endpoint rates interpolate linearly; cumulative overflow starts
only after the inferred balance exceeds the cap used by that update. Measured
loss is the difference of cumulative overflow at the two phase-clipped endpoints.
It is **not** total lost credit multiplied by the fraction of elapsed time in
measurement. This matters when a bucket first fills before or after a phase
boundary. The production profile has a constant rate and cap during measurement.
An interval spanning a ramp transition is still described by the existing
endpoint refill model, not by an independently reconstructed continuous policy.
Unpaced runs have no refill or boundary observations.

Client hold time begins only when a normal NTRN issue decision passes the
active-task, source, canonical-backlog, and pacing gates but lacks client credit
for its intact logical quantum. The existing reason order remains active,
source, canonical, pacing, client, then query. A query-only hold therefore adds
no client time; simultaneous client and query shortages retain client precedence.
The interval ends when a response releases enough client credit, another
observed issue reason replaces it, the observed source is quarantined, no source
is available, or issuance ends. A late drain callback closes the interval at
the configured issue end. Reports include an open interval without committing
it, so subsequent reports do not duplicate duration.

This is an observed scheduler state, not a continuous independent measurement
of every possible source/client combination, and it does not establish that
client capacity caused all pacing loss. Failed runs may stop before publishing
a final hold closure. Hold event counts also depend on pump frequency and must
not be interpreted as durations. To express an aggregate measured hold fraction,
divide worker-seconds by the sum of workers' measured durations, not by one
worker's elapsed time.

`worker_pacing_snapshots[]` includes `worker_id`, `snapshot_elapsed_s`,
`measure_elapsed_s`, `steady_offered`, and `pacing_tokens`. Snapshot elapsed times
share one run start, but the coordinator receives workers' updates
asynchronously. Align per-worker timestamped counts before interpreting changes
between reports as rates. Aggregate periodic gauges are not simultaneous
snapshots. Final offered counts still use the configured measurement duration;
these diagnostics do not alter that denominator or justify relaxing capacity
acceptance gates.

Deterministic regression cases are in
`lite-client/native-load-generator-policy-test.cpp`, under the
`test-native-load-generator-policy` target. They cover clipping at phase
boundaries, fractional refill, invalid clock inputs, repeated open hold
snapshots, ordered hold reasons, and aggregation units. Actor timing and live
telemetry overhead still require the coordinated build and benchmark cycle.
