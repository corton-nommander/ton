# Native admission experiments — 9 September 2026

Status: desktop implementation, profiling and comparison cycles complete. The user approved the measured steps in
[native-bottleneck-action-plan-2026-09-09.md](native-bottleneck-action-plan-2026-09-09.md).
No production deployment or production throughput improvement is claimed here.

The final matched A/B/B/A comparison selected local overlay signature reuse for
the desktop preset: **57,673.19 → 60,039.59 canonical logical TPS (+4.10%)**,
with **9.39% lower sampled validator CPU**. Both candidate runs beat both controls.
Request sharing, snapshot refresh and prepared-key signing remain off. Full
results, the narrow duration reassessment and the resource-environment change
are recorded below. This is the best supported configuration in the final matched
comparison, not a ranking against older images or proof of maximum capacity.

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
it supplies no basis for promotion. Snapshot refresh was subsequently screened
separately with sharing off; its completed comparison appears below.

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
reconciliation-local outcome and stage attribution was subsequently implemented
and measured in the refresh arms below. These initial control counters alone
cannot select an account-read optimization.

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
subsequent refresh comparison used a newly frozen image set with sharing off
and reconciliation stage profiling on in both arms. The completed comparison
below uses that image's own refresh-off control.


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
14 profiler tests pass. The fresh `05-refresh-off` control subsequently passed
with the corrected collector and unchanged validator/generator images.

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
its small potential saving. That cache remains deferred; the later CPU profile
below identifies larger opportunities outside reconciliation.

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
The subsequent `06-refresh-on` arm changed only snapshot refresh to one, with
the same restart policy and prebuilt images. Compact control evidence is saved in
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
The subsequent `07-cpu-diagnostic` capture profiled the refresh-off control under
a separate short load; its throughput is excluded from comparison to these
unprofiled 600-second arms.

## Separate CPU profile and next isolated candidates

The separate `07-cpu-diagnostic` ran 180 measured seconds with the same frozen
refresh-off runtime. Proof/drain, cleanup and image checks passed, but its TPS
is excluded from comparisons because the interval contains CPU profiling. The
60-second raw capture succeeded with stable process identity and zero lost
samples. Initial source-line reporting failed; offline symbol-only regeneration
succeeded without another capture. Original failure and recovery provenance are
retained. See [the CPU report](native-cpu-profile-2026-09-09.md).

The strongest new opportunities are **14.27% sampled user CPU in verification
immediately following local broadcast signing** and **4.53% in repeated keyring
private-key import/public-key derivation**. Actual signing separately accounts
for 4.41%; native admission verification is 5.57% and already uses a positive
signature cache. Candidate metadata decoding accounts for 11.32%, ahead of the
2.38% reconciliation walk. Context percentages overlap and are not TPS forecasts.
There was no validator CPU-quota throttling or memory pressure. Disk write volume
alone does not establish I/O saturation.

The next bounded step-4 experiments are independent, default-off flags:
`TON_KEYRING_PREPARED_SIGNING` reuses an immutable prepared key inside its keyring
owner; `TON_OVERLAY_LOCAL_SIGNATURE_REUSE` reuses cryptographic evidence bound to
the exact successful local signing request/result. Incoming messages retain
signature verification; all other broadcast checks remain. Correctness tests
passed and the shared image set was frozen before independent 600-second
screening began, as recorded below. Reconciliation yields, another native
signature cache and thread-pool changes are deferred while
these stronger measured opportunities are evaluated.

## Signing candidate control

Both profile-led implementations passed focused checks: six prepared-key tests
and eight local-broadcast tests, plus independent source review. Commits
`d921af3d` and `be235e03` isolate the two changes. The local receipt binds independent
copies of the exact Ed25519 request and successful 64-byte signature; mutable
shared buffer aliases and incoming-message identity claims cannot authorize reuse.

The signing comparison uses frozen revision
`be235e037826a36e4f57125d15c73aa0513973af`, with genesis image
`sha256:d0067d546b6bb2c9aeab62a29e30b0264dc97cd1a5a2fe02f947ac102f057758`
and generator image
`sha256:380a0cccbca243c2ba1dc0f14ed0f9b5a6df3fafaf45ead06b596e2195f3f842`.
Sharing/refresh remain off, configuration caching and reconciliation timing on.
The established four-lane, 10-connection, 600-second, 20-ms workload is unchanged.
Both signing candidates were default-off at the start of this comparison.

`08-cpu-control` passed proof/drain, cleanup and strict identities at **57,899.38
canonical logical TPS**, with 57,948.51 offered/admitted TPS. Final backlog,
canonical conflicts, fatal follower errors and exhausted retries were zero;
whole-run transient timeouts were 88 and not-ready responses 609,501. Mean sampled
CPU equivalents were validator 9.98, client 0.94 and Session Stats 0.88. This is a
same-image baseline for the signing experiments, not a cross-version regression
measurement or independently overdriven capacity claim.

Before/after overlay captures occur outside the measured window. Thirteen matched
endpoints recorded an observable lower bound of 6,119,652 cryptographic checks and
zero receipt hits with reuse disabled. Ten overlays retired and ten appeared;
the analyzer therefore does not claim complete interval totals. New-generation
lifetime counters and coverage are saved separately in the compact control result.
The subsequent arm enabled only prepared-key signing.


## Prepared-key signing screen

`09-prepared-signing` completed its 600-second measurement at **54,043.91
canonical logical TPS**, with 54,097.20 offered/admitted TPS: **6.66% below**
`08-cpu-control`. Proof checking, complete drain, canonical cleanup and unchanged
container identities passed; final backlog, hash/follower errors and exhausted
retries were zero. Transient timeouts increased 88 → 1,479, with maximum admission
RTT increasing 4.07 → 10.00 seconds. Mean sampled validator/client CPU was
9.86/0.94 equivalents, versus 9.98/0.94 in the control.

This single screen supplies no basis to enable prepared-key signing by default.
It does not isolate the cause of the lower rate: evolving chain state, scheduling
and desktop variation remain possible influences. The next screen changed only
local broadcast signature reuse relative to the original control. Both candidates
remained off in normal presets at this stage; the later OOM required a fresh
comparison environment before selecting one.

The comparison helper initially rejected the two new runtime flag identities
because the benchmark wrapper's environment whitelist omitted them. The separate
controller saved both flags from Docker inspection, the same container/image IDs,
and a successful unchanged-identity check after each complete run. Raw captures
and the initial failed comparison are retained. The corrected offline comparison uses that
cross-checked controller evidence with explicit per-field provenance and passes
all comparison gates: the sole runtime difference is prepared signing 0 → 1.
Eleven helper tests cover absent/conflicting flags and container-identity mismatches.
No raw wrapper fields were rewritten. The tests still lack independently offered 5%
overdrive and therefore report observed throughput, not a proven capacity limit.


Saved time-series review found two synchronized masterchain/basechain progress
pauses in the prepared-key arm: **22.405 seconds at 07:55 UTC** and **8.030 seconds
at 08:00 UTC**. Of 1,479 admission timeouts, 1,420 clustered around these pauses;
the client then reduced its congestion window. It continued reporting queued
ready work during the first pause, and a validator-control request took 15 seconds.
This establishes stalled validator progress, rather than a dashboard-only gap.
It does not establish that prepared-key reuse caused the stall.

Matching host samples show available memory declining from 20.58 → 5.76 GiB across
control 08, then 5.02 → 2.71 GiB across arm 09 (minimum 1.90 GiB). Mean host full
memory-pressure `avg10` increased 0.460 → 1.783%, with peaks 4.46 → 8.29%.
These are physical desktop host observations, distinct from the Docker VM. A
subsequent check during arm 10 found approximately 65 GiB available inside the
VM and zero guest memory pressure, while the host had roughly 1.55 GiB available
and its 8 GiB swap was full. The VM is configured with a 99,840 MiB shared memory
backend. This resource contention limits causal interpretation of the desktop
comparison; spare guest memory does not establish spare host memory.

At the original control rate, the two pauses represent about 76% of the
whole-window transfer deficit, solely as an illustrative scale comparison.
The reported TPS still includes every measured second; no pause was deleted or
used to manufacture an adjusted result. Detailed evidence is retained in
[the stall analysis](benchmarks/results/native-admission-20260909-prepared-stalls.json).


## Local-signature arm interrupted during final collection

`10-local-signature-reuse` is **excluded from promotion comparisons**. Its generator
finished the full 600-second measurement and proof-checked drain at approximately
08:25:18.575 UTC, with zero final backlog or follower errors. At 08:26:03, about
44.4 seconds later, the host kernel's global OOM killer terminated Docker Desktop's
QEMU process. The wrapper therefore did not finish its complete final collection
and acceptance checks. No accepted TPS or capacity result is assigned to this arm.
The [failure record](benchmarks/results/native-admission-20260909-10-host-oom.json)
retains the successful generator evidence separately from failed wrapper completion.

Docker Desktop was restored with **MemoryMiB 65,536 instead of 99,840**. Only that
setting changed, with a private backup of its previous settings. Existing volumes
and containers were preserved; the application services running before the OOM
were restored, and the load generator remained stopped. This is a new benchmark
resource environment: subsequent controls must not be ranked directly against the
previous 99,840 MiB VM arms. New arm metadata records VM memory/CPU and host total
memory explicitly. The new control used the same frozen `be235e03` images,
10 connections, four lanes, 600 seconds and 20 ms coalescing.

The runtime environment collector now captures both signing flags directly.
Its three projection tests and the benchmark reporting suite pass, including
18 canonical reporting tests and 14 profiler tests. Earlier raw captures remain
unchanged.


## Fresh 64 GiB control and duration reassessment

`11-memory64-control` completed at **57,437.36 canonical logical TPS**, with
57,447.49 offered/admitted TPS. Its generator, proof/catch-up, complete drain,
validator cleanup and immutable-image checks passed. Final backlog, hash/follower
errors and exhausted retries were zero; transient timeouts were 92 and not-ready
responses 659,388. Mean sampled CPU equivalents were validator 10.08, client 0.83
and Session Stats 0.97. This is an observation, without independent 5% overdrive.

The outer sweep initially rejected `measure_elapsed_s=600.0000000000001` using
exact floating-point equality with 600. MyLocal commit `5172686` permits at most
eight floating-point ULPs; integers still compare exactly, and non-finite values,
wrong types and materially short/long durations fail. All 20 sweep tests pass,
including unchanged proof, drain, transfer arithmetic and wrapper-failure gates.

An explicit offline reassessment replays the original assessor on the exact hashed
plan, preflight, launch and benchmark inputs, reproducing its sole duration
rejection. It then applies the corrected assessor to those same bytes. The review
also verifies that executable source differs only in the duration predicate/helper
and that all other runtime harness files are unchanged. Original controller exit 3
and the failed assessment remain recorded; the successful underlying wrapper exit
0 and all other checks are preserved. Five certificate tests and 15 comparison
tests cover input tampering, unrelated source edits and remaining acceptance gates.
The [compact control record](benchmarks/results/native-admission-20260909-11-memory64-control.json)
contains both assessments and their provenance.

The 4,088 captured collation candidates averaged 120.32 ms external wait. All 1,707
probes with confirmed published work returned work, averaging 0.94 ms; the first
epoch wait partitions reconcile. These remain diagnostic candidate wall times,
not exclusive CPU measurements or independently canonical-filtered block metrics.
The next arm isolated local-signature reuse against this control in the same
recorded 64 GiB VM environment.


## First complete local-signature comparison

`12-memory64-local-reuse` passed its full controller, proof/drain, validator cleanup
and image checks at **59,984.77 canonical logical TPS**, versus **57,437.36** for
control 11: **+4.44%**. Offered/admitted throughput was 60,082.16. Both arms use the
same recorded 64 GiB VM, images, topology, load and resource limits. The comparison
accepts only the local-signature flag change and the separately verified duration
postprocessing correction described above.

Matched interior CPU samples were validator 10.07 → 9.48 equivalents (**−5.88%**),
client 0.83 → 0.80 and Session Stats 0.97 → 0.85. Final backlog, proof/hash errors
and exhausted retries stayed zero. Transient timeouts were 92 → 57; the coarse
p50/p95/p99 admission RTT buckets stayed 200/500/1,000 ms. This single screen is
promising, but it is not yet a repeatable promotion or an independently overdriven
capacity result.

Eight matched overlay endpoints recorded at least **6,345,284 receipt hits**, zero
receipt mismatches and 136 remaining crypto checks. Fifteen endpoints retired and
fifteen appeared, so these are observable lower bounds, not complete interval
totals. The counter evidence confirms that the intended local rechecks were avoided.
Incoming verification and other broadcast validation paths remain unchanged.

The [candidate record](benchmarks/results/native-admission-20260909-12-memory64-local-reuse.json)
is retained. The candidate and then the control were repeated with a restart
before each arm and strict reuse of the existing images, completing A/B/B/A
before selecting settings.


## Local-signature repeat

`13-memory64-local-repeat` passed full collection, proof/drain, cleanup and image
continuity at **60,094.40 canonical logical TPS**, with 60,061.73 offered/admitted
TPS. Its canonical result is **0.18% above** the first candidate's 59,984.77 TPS.
Final backlog, hash/follower errors and exhausted retries stayed zero; transient
timeouts were 106 and not-ready responses 595,733. The coarse p50/p95/p99 RTT
buckets again stayed 200/500/1,000 ms.

Mean sampled validator/client/Session Stats CPU was 9.17/0.74/0.98 equivalents.
Eight matched overlay endpoints recorded an observable lower bound of 6,397,711
receipt hits, zero mismatches and 127 remaining crypto checks; fifteen endpoints
retired and fifteen appeared. Full interval totals remain unproven.

The [repeat result](benchmarks/results/native-admission-20260909-13-memory64-local-repeat.json)
is retained. The final arm returned local-signature reuse to zero under the same
64 GiB VM environment, images, restart policy and 600-second/20-ms workload.
The desktop preset was selected only after that final control completed.


## Final control, A/B/B/A and desktop selection

`14-memory64-control-repeat` completed at **57,909.02 canonical logical TPS**,
with 57,910.64 offered/admitted TPS. Full controller/wrapper collection, immutable
image checks, proof/catch-up, complete drain and validator cleanup passed. Final
backlog, hash/follower errors and exhausted retries were zero; there were 63
transient timeouts and 630,257 not-ready responses over the whole run. The
[final control record](benchmarks/results/native-admission-20260909-14-memory64-control-repeat.json)
is retained alongside the other arms.

| Order | Local signature reuse | Canonical logical TPS | Offered/admitted TPS |
| --- | --- | ---: | ---: |
| A1: 11 | 0 | 57,437.36 | 57,447.49 |
| B1: 12 | 1 | 59,984.77 | 60,082.16 |
| B2: 13 | 1 | 60,094.40 | 60,061.73 |
| A2: 14 | 0 | 57,909.02 | 57,910.64 |
| Control mean | 0 | **57,673.19** | |
| Candidate mean | 1 | **60,039.59** | |

The [aggregate evidence](benchmarks/results/native-admission-20260909-overlay-ABBA.json)
checks chronological, non-overlapping A/B/B/A windows, all four cross-mode
comparisons and both same-mode repeats. Images, workloads, resource settings and
recorded host/VM environments match; only the reuse flag differs, plus the
explicitly certified duration-only postprocessing correction for arm 11. Its
original controller exit 3 remains preserved. Every arm completes 600 measured
seconds with 20 ms coalescing, all cohorts proven and no final backlog.

Mean canonical TPS improves **4.103%**. Both candidates exceed both controls;
individual cross-pair gains range from **3.585% to 4.626%**. The full control
range is 0.818% of its mean, and the candidate range is 0.183%. This passes the
predeclared desktop selection screen: both candidates above both controls, mean
gain at least 2%, and gain greater than the full control repeat range. Four
observations do not establish formal statistical significance.

Using the aggregate's consistent measured-interior sampling policy, mean
validator CPU equivalents fall **10.3185 → 9.3491 (−9.39%)**. Dividing sampled
CPU by canonical throughput estimates **178.90 → 155.72 CPU microseconds per
transfer (−12.96%)**; this is not a direct native-execution timer. Earlier per-arm
CPU means use slightly different sample boundaries and are preserved as recorded.
Admission p50/p95/p99 RTT buckets remain 200/500/1,000 ms in all four arms.

The evidence supports avoiding redundant verification immediately after an exact
local keyring signing callback. Incoming network verification and the remaining
broadcast checks are preserved; see [the feature contract](overlay-local-signature-reuse.md).
Candidate endpoint counters show millions of receipt hits and zero mismatches,
but rotations and warmup/drain coverage make those lower bounds rather than full
measured-window totals.

MyLocalTonDocker commit `4521217` persists the tested `.env.desktop` configuration:

```dotenv
TON_BRANCH=admission-local-be235e03
TON_IMAGE=mylocalton-ton
MLT_IMAGE=mylocalton-genesis
NATIVE_LOAD_IMAGE=mylocalton-client:admission-local-be235e03
TON_KEYRING_PREPARED_SIGNING=0
TON_NATIVE_ADMISSION_SHARD_SHARING=0
TON_NATIVE_ADMISSION_SNAPSHOT_REFRESH=0
TON_NATIVE_RECONCILIATION_PROFILE=1
TON_OVERLAY_LOCAL_SIGNATURE_REUSE=1
```

The resolved Compose configuration equals the tested final-control configuration
except for the promoted overlay flag. The genesis/client IDs remain
`sha256:d0067d546b6bb2c9aeab62a29e30b0264dc97cd1a5a2fe02f947ac102f057758`
and `sha256:380a0cccbca243c2ba1dc0f14ed0f9b5a6df3fafaf45ead06b596e2195f3f842`.
The source is `be235e037826a36e4f57125d15c73aa0513973af`; later commits save
results and harness corrections without rebuilding these binaries. Docker Desktop
retains its corrected 64 GiB allocation. No database reset or image rebuild is
needed to select the winner.

Physical-server and C++/Compose fallback defaults remain off. These CPU-specific
local images have not been published. Deploying on Server A requires a published
image containing the feature and a matched test on its existing eight-lane chain.
Unique offered/canonical ratios in the desktop arms are approximately 1.0, below
the required independent 5% overdrive: this remains a repeatable throughput
improvement, without establishing an upper capacity limit or a 200k TPS forecast.

## Remaining work selected by evidence

Already-published work reaches the desktop collator in about 0.7–0.9 ms; the
100+ ms external-wait total includes other pipeline and packing stages. A delivery
rewrite is therefore deferred. Sharing and refresh reduce redundant work but
showed no TPS gain; prepared signing has no clean positive comparison and remains
off after its host-memory-confounded screen.

The next bounded target is candidate metadata extraction: the separate CPU
profile attributes 11.32% of sampled user CPU to native message decoding during
state resolution. A metadata-only decoder could avoid expanding full transfers
and unused account structures, while retaining malformed-input checks, exact
parent hashes and branch isolation. This is a proposed next experiment, not
implemented or assigned a predicted TPS gain. The serialized external-message
pool also consumes about 0.85 sampled core and needs queue/long-turn attribution
on the physical servers. Matching Server A/B profiles and a production A/B test
remain outstanding before considering the separately gated lane-owned redesign.
