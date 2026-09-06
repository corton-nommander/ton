# Pacing audit during the predeclared 180s ABBA series

Read-only source and completed-diagnostic audit; no pacing policy, tracked
source, build, tests, harness, or running experiment changed. Preserve the series
and all existing load/capacity gates before considering any follow-up.

Evidence bundle:
/home/neodix/gitProjects/MyLocalTonDocker/benchmark-results/cycles-20260906-coalesce-diagnostic-55k-cwnd12288/

Confirmed arithmetic and bounds

- native-load-generator.log:156 reports target55000, steady_offered3212496,
  measure_elapsed_s60, offered53541.6TPS, attainment0.9734836363636363.
  The nominal3300000 budget exceeds actual issuance by87504logical (2.6516%).
- No measurement-cohort bug found. All workers share one monotonic start; rates
  are distributed as doubles proportional to source count (generator.cpp:1282).
  Each parent checks the half-open measurement interval before issuance
  (:3559), counts its whole logical_count (:3761), and is summed by coordinator
  (:1354). Elapsed time is the maximum common-worker measure duration (:1485)
  clamped to the configured end (:5032), and final TPS is count/duration (:1642).
  The59s canonical gen_utime bucket affects canonical TPS only.
- Fixed16-output quanta do not discard credit each tick. update_tokens (:3242)
  preserves fractional credit; only actual issued logical_count is subtracted
  (:3566). At55000/6 each worker earns one16-output parent per1.745ms. A nominal
  10ms tick can issue5 or6 parents with carry. Pure endpoint quantum residue is
  less than6*16=96logical (<1.6TPS over60s), not87504.
- The token bucket is capped at100ms of target (:3233-3250), about916.667 tokens
  per worker/5500 summed. Excess refill is silently discarded. Consequently
  ordinary fractional rounding or one bounded bucket of unused endpoint credit
  cannot explain the observed deficit. Long/repeated issuance stalls and/or
  delayed boundary service are needed; no exact clipped-credit total is recorded.
- alarm (:3192) schedules its next10ms wake after pump work. Elapsed-time refill
  compensates scheduling/processing delay until the100ms cap clips. An on-time
  10ms timer alone does not produce a repeated2-3percent loss. Real callback or
  timer delays are unmeasured here, so10ms is not a guaranteed wall-clock bound.

Residual pressure visible in completed samples

- Total issue holds: pacing531562, client21415, other four categories zero.
  Between report snapshots at elapsed89.675 and149.552 (lines92 and150), client
  holds rose13807 and pacing holds225352. This is a measurement-spanning snapshot
  interval, not an exact synchronized per-worker measurement delta.
- log:125: inflight12288 (full), ready1760, aggregate pacing tokens4700.27.
  log:136: inflight12288, ready1712, tokens4994.23. This shows usable credit and
  signed work waiting behind the client limit even after coalescing improved
  average admission overhead. No aggregate sample reached5500; that cannot rule
  out individual worker buckets clipping between/asynchronously across reports.
- Final RTT buckets p50/p95/p99=50/500/500ms, maximum1122.188ms (:156).
  At55000TPS, global cwnd12288 represents about223ms of nominal offered work;
  the latency tail can still exhaust individual windows despite spare average
  CPU and a much lower physical-query rate.
- All34114 server errors/retries are not_ready; there are no repairs, quantum
  violations, transport errors, or retry exhaustion. 490788physical attempts
  versus456674 signed parents represent7852608 versus7306784logical transfers:
  retries add545824logical attempts, 7.47percent over original all-run issuance.
- Hold counters measure blocked opportunities, not duration. Batching reduces
  callbacks and pump invocations; fewer client-hold events do not establish a
  proportionate decrease in time spent unable to issue.

Limits of the available evidence

- Periodic reports aggregate asynchronous latest-worker snapshots (:1346), with
  independent worker publish_stats (:5040). Apparent per-report rates swing
  19477.94-78493.49TPS. Those swings cannot be read as synchronized one-second
  pacing gaps. Final counts and fixed60s denominator are reliable.
- The final5500 pacing-token gauge is AFTER drain. Drain callbacks continue to
  refill/clamp it; it is not evidence of the token balance at measurement end.
- CPU0.734cores is an average, not a maximum worker scheduling delay. The existing
  data supports residual client-window stalls plus finite-bucket clipping as a
  plausible mechanism, but does not quantify which share of87504 came from
  clipping, scheduler/pump delays, or endpoint timing. No new correctness bug
  or exact attribution is established.

Minimal telemetry that would disambiguate a later diagnostic

1. Per-worker measured clipped-token amount/count from update_tokens, with exact
   phase-overlap attribution and pacing balance/time at measurement start/end.
2. Maximum/histogram of time between token updates, pump duration, and scheduled
   alarm lateness; measure boundary service lag explicitly.
3. Duration spent unable to issue a whole quantum due to client credit, separate
   from source/canonical/query/pacing holds; per-client RTT/inflight evidence.
4. Per-worker snapshot timestamps, so rates/gauges can be aligned rather than
   interpreting aggregate asynchronous report swings as scheduler stalls.

Do not change target, denominator, acceptance gates, or pacing policy during the
current ABBA series. This document is a durable pointer for the next continuation.
