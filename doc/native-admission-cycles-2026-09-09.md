# Native admission experiments — 9 September 2026

Status: in progress. The user approved the measured steps in
[native-bottleneck-action-plan-2026-09-09.md](native-bottleneck-action-plan-2026-09-09.md).
No production deployment or production throughput improvement is claimed here.

## Comparison conditions

The desktop reference has four fixed payment lanes, 24,576 sources, 10 persistent
submission connections, six client workers/signers, 16 logical transfers per
signed parent, batch limit 64, 20 ms coalescing, 60 seconds of warmup and 600 seconds
of measurement. The initial/maximum congestion windows are 32,768/65,536 logical
transfers. The validator has a 16 CPU quota and the client six CPUs; the desktop
Docker VM has 24 vCPUs. Existing unrelated desktop applications remain running.
Configuration caching is on; snapshot refresh is off for the sharing comparison.

Every measured arm uses prebuilt images with strict identity checks. The initial
sharing pair uses source `70cd547b0e75a6e2b258212f3b4d9d0613a39885` with a local
Release build tuned to this desktop CPU and a bundled, fixed userspace runtime.
It is not the portable September 8 image: compare this day's paired controls,
not the absolute rate against the older binary. Image and binary provenance is
saved under `build/benchmarks/admission-cycles-20260909/runtime-70cd547b/`.

- Genesis image: `sha256:77088355441893a4c215f7227eb28ee039a69cae6b1ba2597cc34ea3575c80af`.
- Generator image: `sha256:1c3a6799cd804b6d1a112c02931419a31d0d466ed66cc3caf057674d1a447032`.
- The generator includes the corrected unpaced capacity classification. Throughput
  remains an observation when offered load does not independently exceed canonical
  throughput by at least five percent.

The database advances between arms; no source keys, volumes or chain state are
reset within the comparison. The validator is restarted between configurations,
then checked healthy, before the generator's readiness and warmup stages. There
are no image builds or CPU-heavy tests during measured arms. Promising candidates
need repeated controls before promotion.

## Completed measurements

| Arm | Sharing | Offered/admitted logical TPS | Canonical logical TPS | Proof/drain | Capacity claim |
| --- | --- | ---: | ---: | --- | --- |
| `02-sharing-off` | Off | 59,834.48 | 59,833.64 | Complete | No independent overdrive |

`01-sharing-off` failed image preflight before any generator or traffic started;
its setup artifacts are retained and it is not a performance measurement.

The completed control has zero final backlog, canonical hash conflicts,
proof-follower errors or exhausted source retries. There were 75 transient client
timeouts and 746,868 not-ready admission responses over the complete run. These
whole-run counts include warmup and drain; the stage data below uses only sampled
intervals fully inside the measured window.

## Control diagnostics

The 570-second interior profile captured 62,779 manager requests, 3,717 cache fills
and 58,518 fill races. Completed-state caching does not combine simultaneous
requests; the sharing candidate targets this duplication. The underlying manager
already shares state loading, so these are not 58,518 redundant disk reads.

Mean batch admission residence was 110.0 ms; shard wait was 53.0 ms and verification
stage wall time 89.8 ms. These stages overlap and include asynchronous scheduling.
Snapshot changes rejected 19.96% of completed physical inputs. Configuration
cache hit fraction was 99.94%; mean uncached extraction was only 0.022 ms.

Delivery attribution covered all 5,834 diagnostic basechain collated candidates
in the measurement interval. These candidates are not independently filtered to
the canonical chain and must not be used as the canonical TPS denominator.
External wait averaged 114.4 ms/candidate. Of first-work wait, 467.3 aggregate
seconds began without a pending producer epoch and 30.4 seconds with one. A pending
epoch means unfinished producer work, not necessarily admissible ready work.
All 1,649 probes with confirmed prior publication returned work; their mean wall
time was 0.752 ms. This observation favors investigating upstream availability
and scheduling before changing the published queue handoff.

Mean sampled Docker CPU equivalents were 9.95 for the validator, 0.82 for the
client and 0.49 for session-stats. These sampled values do not exclude a saturated
individual actor. Host `/proc` metrics from the legacy harness do not identify
Docker Desktop VM tasks reliably and are excluded. Actor occupancy is also not
an exclusive OS CPU profile.

Canonical reconciliation performed 10,476,301 account lookups in the interior
interval. Its existing `sources_advanced` counter also includes admission paths;
it cannot establish the fraction of redundant reconciliation reads. Additional
reconciliation-local outcome and stage attribution is being prepared before
selecting an account-read optimization.

Block-signature validation created about 68.3 helper threads/s. Measured launch
and join wall-time sums were 19.49 and 9.39 seconds over the interior interval;
these do not establish a dominant CPU cost and do not justify a worker rewrite
on their own.

## Artifacts and checks

Raw logs, frozen images, final generator proofs, strict-reuse validation, dashboard
corroboration and profile snapshots remain in
`build/benchmarks/admission-cycles-20260909/<arm>/`. Each completed arm has
`analysis.json` and `delivery-analysis.json`. Canonical rates use fully contained
block `gen_utime` buckets: 599 complete seconds inside each 600-second wall window.

Before this comparison, the sharing implementation passed 81 scheduler tests,
eight admission tests and nine collator-wait tests. Coverage includes exact-state
fanout, fork identity, caller deadlines/cancellation, caps, stale generations,
early manager errors and actor teardown. The validator Release build and fixed
runtime image executable checks passed. Snapshot refresh is not enabled until
its actual batch-coroutine tests and a separate frozen-image comparison pass.
