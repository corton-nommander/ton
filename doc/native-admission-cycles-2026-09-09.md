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
| `03-sharing-on` | On | 59,244.64 | 59,218.56 | Complete | No independent overdrive |

`01-sharing-off` failed image preflight before any generator or traffic started;
its setup artifacts are retained and it is not a performance measurement.

The completed control has zero final backlog, canonical hash conflicts,
proof-follower errors or exhausted source retries. There were 75 transient client
timeouts and 746,868 not-ready admission responses over the complete run. These
whole-run counts include warmup and drain; the stage data below uses only sampled
intervals fully inside the measured window.

## Sharing decision

Sharing remains default-off: the candidate observed **1.03% lower canonical TPS**
than its control. Both are valid completed observations, not overdriven capacity
claims. This single pair cannot resolve small effects or prove a regression, but
it supplies no basis for promotion. Snapshot refresh will be screened separately
with sharing off.

In approximately matched 570-second interior profiles, actual manager requests
fell from 62,779 to 3,325 (**94.7% fewer**), with 57,693 shared joins. Duplicate
cache fills fell from 58,518 to 31. No sharing-table or waiter cap was reached.
There were 93 bounded later-deadline fallbacks; these preserve caller deadlines.
The substantial request saving did not improve throughput or batch wall latency:
mean residence increased from 110.0 to 129.8 ms and snapshot-change rejection
from 19.96% to 22.32% of completed inputs. Mean block packing increased from
6,281 to 7,890 logical transfers. These associations do not isolate the cause
of the changed timing; request count alone is not a throughput proxy.

Full-run transient timeouts were 75/109 and not-ready responses 746,868/798,351
for off/on; neither arm exhausted a source retry or retained final backlog.
Mean sampled validator CPU was 9.95/10.04 equivalents. Both fixed images and
validator identities passed the harness checks. Checked-in compact evidence is
in `doc/benchmarks/results/native-admission-20260909-0{2,3}-sharing-*.json`.

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
runtime image executable checks passed. Snapshot refresh subsequently passed all eight actual batch-coroutine cases,
including real Ed25519 verification, exact state/header decoding and reservation
counts. All 122 focused tests passed: refresh integration 8, refresh policy 8,
admission 8, pool scheduler 85 and collator wait 13. The MyLocal profiler passed
14 tests. These correctness results allow an opt-in performance screen; they do
not establish a throughput gain or justify a default change.

Isolated implementation commits are `7bcf9b99` (bounded batch refresh), `09c625a6`
(reconciliation attribution) and `c2843fe4` (first-epoch delivery timing). The
subsequent refresh comparison will use a newly frozen image set with sharing off
and reconciliation stage profiling on in both arms. Its results must be compared
to that image's own refresh-off control.


## Refresh control collector failure (retained)

`04-refresh-off` used fixed revision `f5978d11` with sharing/refresh off and
reconciliation timing on. Its generator completed 600 measured seconds at
58,435.82 canonical logical TPS with zero final backlog, hash conflicts, fatal
follower failures or exhausted source retries. The wrapper nevertheless exited 3:
its old substring parser selected `total.ext_msg_native_reconciliation_diagnostics`
instead of the exact `total.ext_msg_native_reconciliation` key. It consequently
reported missing cleanup telemetry.

The original raw before/after captures contain the correct key. A separate
exact-key recheck through the unchanged cleanup acceptance function passes:
`pending_sources=0`, pending native accounts/messages/logical messages all zero.
The failed original assessment is preserved; this arm is excluded from promotion
comparisons. MyLocal commit `5829f7a` fixes exact first-field matching and tests
prefix collisions, whitespace, absent/repeated samples. The reporting suite and
14 profiler tests pass. A fresh control will run with the corrected collector;
no validator or generator image change is needed.

Its additive measurements remain useful diagnostics: 4,772 captured basechain
candidates have fully reconciling first-epoch wait partitions. Of 353.54 aggregate
seconds entered before installation, 163.75 seconds precede the first epoch and
189.79 follow it. Pre-epoch overlap is 34.32 ms/candidate; it includes dispatch and
early installation work, not exclusively mailbox latency. Later completed-epoch
wait adds 48.93 seconds. This motivates CPU stacks and bounded scheduling review,
not removal of all external waits.

The 540-second interior reconciliation profile shows 81.93% unchanged account
observations, but only 0.0208 unpack wall seconds per elapsed second (1.14 µs/read).
Application/reservation work is 0.1358 s/s and dictionary lookup 0.0540 s/s.
Manager waiting is asynchronous wall time, 0.3470 s/s. Unchanged account facts
are not an exact-content cache hit rate, and decode-cache overhead may offset
its small potential saving; that cache remains deferred pending CPU stacks.

## Fresh refresh control

`05-refresh-off` completed with the corrected collector: **58,962.64 canonical
logical TPS**, 58,661.49 offered/admitted TPS, 600 measured seconds and 599 complete
block-time buckets. Generator proof/drain and wrapper cleanup/strict identity
checks all passed, with zero final backlog, hash conflicts, fatal follower errors
or exhausted source retries. There were 113 transient timeouts and 584,332
not-ready responses over the whole run. Offered and canonical rates use different
cohorts/time buckets; their small difference is not an accounting failure. There
is no independent five-percent overdrive and no capacity claim.

This pair uses frozen source `f5978d11b52f649ce8d464ce050440963306555b`:
- Genesis: `sha256:cc1d01ad8654c17b61f2ef995c188ad065724f8c5bae2e56520a78ed6b9f6549`.
- Generator: `sha256:38a859d7d8552f99e6961d301fac728d369984bd1f222a63efd7eda3f7e3fed9`.

Sharing is off and reconciliation timing is on in both refresh arms. Mean sampled
CPU equivalents were validator 10.24, client 0.88 and session-stats 0.69. Delivery
partitions reconcile across 4,621 diagnostic candidates: external wait 111.93
ms/candidate, of which first-epoch pre-installation overlap accounts for 31.55
ms/candidate. All 1,928 already-published probes returned work, averaging 0.72 ms.
These candidate timings are diagnostic wall times, not canonical CPU attribution.
The next arm changes only snapshot refresh to one, with the same restart policy
and prebuilt images. Compact control evidence is saved in
`doc/benchmarks/results/native-admission-20260909-05-refresh-off.json`.

## Refresh decision

| Same-image arm | Refresh | Offered/admitted TPS | Canonical TPS | Proof/drain and strict cleanup |
| --- | --- | ---: | ---: | --- |
| `05-refresh-off` | Off | 58,661.49 | 58,962.64 | Pass |
| `06-refresh-on` | One bounded retry | 57,836.21 | 57,904.05 | Pass |

Refresh remains default-off. The candidate observed **1.80% lower canonical TPS**;
this single pair establishes no repeatable gain or definite small regression.
Neither candidate produced a promising TPS screen requiring promotion repeats.
All raw results are retained. No independent five-percent offered overdrive was
present, so these are completed throughput observations, not capacity limits.

Whole-run not-ready client responses fell 584,332 → 2,490 (**99.57% fewer**).
The matched interior rejection fraction fell 16.5746% → 0.0683% of completed
physical inputs. All three later account-change rejection counters stayed zero;
canonical-watermark lag contributed 4/10 responses in the interior off/on windows.
Thus the reduction was not displaced into those later rejection categories.
Both runs had zero final backlog, hash conflicts, fatal follower errors and
retry exhaustion; transient timeouts were 113/114.

The additional pass still incurs work: masterchain pins per completed batch
increased 1.000 → 1.162, shard-state requests 3.408 → 3.915 and manager asks
0.287 → 0.374. Mean admission residence increased 101.67 → 113.05 ms. Verification
wall time per completed batch increased 85.87 → 92.36 ms even though its mean per
pass fell; refresh creates extra passes, including signature-reused ones. These
are overlapping wall-stage observations, not exclusive CPU or causal allocation.

The candidate recorded 28,516 refresh attempts, 28,394 successful completed
batches, 112 exhausted refreshes, 24 deadline events and 341,383 signature-evidence
reuses in its interior window. These populations overlap; refresh accepted-input
counts include exact-hash idempotence and are not new canonical transfers. Sample
boundaries also carry active batches. The full counter definitions and matched
normalizations are retained in
`doc/benchmarks/results/native-admission-20260909-refresh-comparison.json`.

Mean sampled validator/client CPU was 10.24/0.88 off and 10.17/0.86 on. Session
Stats varied 0.69 → 1.02 CPU equivalents; unrelated desktop work and evolving chain
state remain sources of variation. The implementation is available for explicit
retry-focused trials, but lower retry counts alone do not justify default promotion.
The next capture profiles the refresh-off control under a separate short load;
its throughput is excluded from comparison to these unprofiled 600-second arms.
