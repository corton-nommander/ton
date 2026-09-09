# Strict candidate metadata projection — 9 September 2026

Status: implementation, correctness checks, microbenchmark and four 600-second
measurements complete. The desktop preset selects the projection, retaining
local overlay reuse and the existing load parameters. Production defaults stay
off; the new local images have not been published.

| Setting | Canonical TPS, first run | Canonical TPS, repeat | Mean |
| --- | ---: | ---: | ---: |
| Full metadata decoding (0) | 59,784.97 | 60,618.60 | 60,201.79 |
| Parent projection (1) | 61,380.86 | 62,537.54 | 61,959.20 |

The observed mean gain is **2.92%**; both candidates exceeded both controls.
All four runs passed all 59 proof, drain, completion and identity checks. Mean
sampled validator CPU was 4.59% lower, with substantial variation between
controls. Rebuildable-cache cleanup preceded the final control; this limits
causal interpretation and is disclosed below. The result supports a bounded
desktop selection, not a maximum-capacity or production throughput claim.
The [saved evaluation](benchmarks/results/native-candidate-metadata-evaluation-20260909.json)
contains the numeric screen, arm checks and maintenance evidence. This follows
the separately completed [admission/signing cycle](native-admission-cycles-2026-09-09.md).

## Change and validation boundary

The separate [CPU capture](native-cpu-profile-2026-09-09.md) attributed 11.32%
of sampled validator user CPU to native candidate metadata decoding. The old
caller parsed a complete batch, flattened every signed run into child execution
entries with copied signatures, derived an account set, then rebuilt each
parent's external cell to recover its hash. Consensus metadata uses only the
parent hash, source and nonce interval.

`NativeTransferBatch::unpack_external_metadata` returns these immutable records
through a dedicated API. It shares the strict parser with complete `unpack`.
For direct-run versions 5/6 it avoids child execution entries, retained runs and
the derived account table, while using the hash from the already reconstructed
and checked canonical parent root. An explicit bounded logical count replaces
the materialized entry count. Header limits, root/count consistency, fields,
tree weights and tags, signatures' wire format, nonce bounds, trailing-data
rejection, and canonical parent and batch tree comparisons remain enforced.
Scalar versions 1–4 retain the full parser and existing external-hash projection.

Neither parser performs cryptographic signature authentication or account-state
validation; those checks remain in their existing admission/validation paths.
The private parser scratch cannot escape as a partially populated execution
object. Errors return no partial metadata. Consensus retains the existing
workchain handling, sort/dedup, opaque/non-native handling, exact-parent nonce
floors and fork isolation. No cross-candidate cache or canonical-watermark change
is included.

`TON_NATIVE_CANDIDATE_METADATA_PROJECTION=1` selects the new path when the
normal extractor first reads the process environment; that choice is cached.
Only literal `1` enables it, and the default is 0. An explicit internal
mode allows deterministic differential tests without manipulating process state.
Compose, runtime capture and the validator profiler record the flag.

## Measurement protocol

Prepare new binaries/images once after correctness checks. Both control and
candidate use those exact images, the same existing four-lane database and the
retained local-signature winner (`TON_OVERLAY_LOCAL_SIGNATURE_REUSE=1`). Keep
configuration caching and reconciliation clocks on; prepared signing, admission
sharing and snapshot refresh stay off.

Each arm uses 24,576 sources, 10 persistent connections, six workers/signers,
16 logical transfers per parent, batch limit 64, 20 ms coalescing, 60 seconds of
warmup, 600 measured seconds and up to 180 seconds of drain. Initial/maximum
admission windows stay 32,768/65,536. The validator/client quotas remain 16/6
CPUs and the Docker Desktop VM remains at 64 GiB with 24 vCPUs. Restart and
settle genesis identically before each arm, preserve the database, and prohibit
image changes during measurements.

Start with projection off/on; repeat a promising candidate and then the control
to form A/B/B/A. Select a setting only after full proof, final catch-up, complete
drain, cleanup and identity checks. Compare against its same-image controls;
the desktop selection screen requires both candidate runs above both controls,
at least 2% mean TPS improvement and a gain exceeding the full control range.
Older absolute TPS figures are not a controlled comparison. Unique offered
load must independently exceed canonical throughput before a capacity claim.
Preserve failures and keep the existing desktop preset if the candidate has no
repeatable improvement.

Microbenchmarks separately compare BOC decoding, full parsing, parent projection,
flattening, account collection and parent-hash reconstruction. Their stages
overlap and do not add up to exclusive CPU attribution or predict validator TPS.

## Correctness and decoder timing

Independent parser and caller reviews found no validation mismatch. The new
10-test codec suite passes, including both direct versions, scalar versions,
empty/duplicate/short parents, maximum logical and parent counts, malformed
headers/trees/signature cells, nonce overflow, trailing data and equivalent but
noncanonical encodings. The 11-test consensus metadata suite passes with the
process flag both off and on, preserving parent identity, deduplication, nonce
floor results, opaque/non-native handling and terminal-nonce failure behavior.
The existing 18 native-state tests and state-resolver policy checks also pass.
The first test compilation found fixture/helper naming errors; those were fixed
before running these checks. Production code compiled without that failure.

The microbenchmark completed 216 stage records with 15 timed repetitions per
record after warmup. Representative version-6 batch-BOC medians for repeated
identical outputs within each 16-transfer parent are:

| Logical transfers | Full decode + parent hashes | Parent projection | Time reduction |
| ---: | ---: | ---: | ---: |
| 64 | 59.99 µs | 34.98 µs | 41.69% |
| 4,096 | 4,257.59 µs | 2,355.55 µs | 44.67% |
| 16,384 | 18,210.60 µs | 10,041.56 µs | 44.86% |
| 65,536 | 93,411.05 µs | 44,169.67 µs | 52.71% |

The run also covers version 5 and distinct-output parents. This is a warm-cache
batch-BOC microbenchmark, including object destruction; it excludes the complete
candidate Block wrapper, cryptographic authentication and the rest of the
validator. These reductions are not TPS percentages. Source and binary hashes,
all stage records and comparison details are saved with the microbenchmark
[evidence](benchmarks/results/native-candidate-metadata-micro-20260909.json);
full-run performance must be measured separately.

## Frozen runtime

The runtime was built from source commit
`7b73cdb157a1f5e621e420e1e5c913726b267cf5` with Release clang++ 22 and native CPU
tuning. The validator reports that exact revision. The local image wrappers
retain a copied, hashed userspace runtime; they are not portable published images.

- TON base: `mylocalton-ton:admission-local-7b73cdb1`,
  `sha256:3ebc57f991013c8ef5a42c2199a141feed73947bbdbb3ea80f3915aff62cab5e`.
- Genesis: `mylocalton-genesis:admission-local-7b73cdb1`,
  `sha256:80bce954af67215b66a05e1ffc3a1a893775180773ad77b717b672e5ce80b6c1`.
- Generator: `mylocalton-client:admission-local-7b73cdb1`,
  `sha256:e2932e7731c6d5f0c546942dcb7a6e7ed1c50ecf7e0bde408c57a17e2cc9e4b8`.

The experimental `env.cpu` keeps overlay signature reuse on in both arms and
changes only metadata projection from 0 to 1. `.env.desktop` now retains these
images with metadata projection enabled, as selected below. The existing
four-lane database is preserved and the generator remains stopped during startup.

## Control measurement

`01-metadata-control`, projection 0 with overlay reuse 1, passed the complete
controller and wrapper at **59,784.97 canonical logical TPS**, with 59,821.23
offered/admitted TPS. It proved 35,811,200 transfers in the 599 fully contained
integer block-time seconds of its 600-second measurement. Proof/catch-up, full
drain, validator cleanup and strict image/container identity checks passed;
final backlog, canonical hash/follower errors and exhausted retries were zero.

Mean interior sampled CPU equivalents were validator **9.34**, generator 0.83
and Session Stats 0.94. Admission p50/p95/p99 RTT buckets were 200/500/1,000 ms.
Whole-run counters included 69 transient timeouts and 638,989 not-ready replies;
those counts include warmup and drain. The sampled canonical-backlog peak was
830,096, with no final backlog. Offered/canonical throughput was approximately
1.0, so this remains an observation without an independently overdriven capacity
claim. The [control record](benchmarks/results/native-candidate-metadata-20260909-01-metadata-control.json)
is retained.

## First candidate measurement

`02-metadata-projection` passed the complete controller/wrapper at **61,380.86
canonical logical TPS**, with 61,654.45 offered/admitted TPS: **+2.67%** relative
to control 01. The independent comparison accepted all arm checks, immutable
image equality, recorded VM/host resource equality and the sole metadata 0→1
configuration difference. It counted 36,767,136 canonical transfers across 599
fully contained block-time seconds. Both cohorts drained completely; hash and
follower errors and exhausted retries remained zero. Whole-run transient
counters were 40 timeouts, 682,815 not-ready replies and 485 too-old replies;
these are not canonical proof conflicts.

The comparison helper's consistent interior sampling policy reports validator
CPU equivalents **9.3273→8.3295 (−10.70%)**, generator **0.8163→0.9187** and
Session Stats **0.9386→0.9514**. These are sampled rate estimates, not integrated
CPU time. The earlier control diagnostic average above uses a slightly different
sample boundary; paired comparisons use the helper's same policy for both arms.
Native packing was 10,881.07 transfers/block, RTT p50/p95/p99 buckets were
500/500/1,000 ms, and sampled canonical backlog peaked at 1,223,120. Larger
backlog and not-ready counts mean that lower decoding CPU does not eliminate
admission/delivery delays. Offered/canonical throughput was 1.0045, so this is a
controlled observation, without a maximum-capacity claim.

The [candidate record](benchmarks/results/native-candidate-metadata-20260909-02-metadata-projection.json)
is saved, with the full comparison in `01-vs-02.json` under the raw artifact
root. This first gain triggered a candidate repeat and then another control; on its
own it did not establish repeat consistency. Genesis was explicitly recreated
before the same-flag candidate repeat, preserving the database and frozen images.

## Candidate repeat

`03-metadata-repeat` passed all 59 arm checks and the full controller/wrapper at
**62,537.54 canonical TPS**, with 62,524.27 offered/admitted TPS. Its 37,459,984
canonical transfers occupy 599 fully contained block-time seconds. Both cohorts
fully drained, final catch-up completed, and hash/follower errors and exhausted
retries remained zero. The same-mode comparison finds identical configuration,
images and recorded resources to candidate 02.

Interior sampled CPU equivalents were validator **8.7190**, generator **0.7640**
and Session Stats **0.9016**. Whole-run counters include 89 timeouts and 484,260
not-ready responses. Native packing averaged 13,499.09 transfers/block; RTT
p50/p95/p99 buckets were 200/500/1,000 ms. Sampled canonical backlog peaked at
1,396,464 and finished at zero. Offered/canonical ratio was 0.99979, so there is
still no independently overdriven capacity claim. Canonical block-time buckets
and offered cohorts have different boundaries; a small canonical/offered
rate difference does not imply unproved or duplicated transfers.

The [repeat record](benchmarks/results/native-candidate-metadata-20260909-03-metadata-repeat.json)
is saved. Both candidate runs exceed control 01; the completed final control
and desktop selection follow.

## Final control and interpretation

`04-metadata-control-repeat` completed at **60,618.60 canonical TPS**, with
60,505.68 offered/admitted TPS. It passed all 59 checks, full cohort drain and
final catch-up, with zero hash/follower errors and exhausted retries. The
[final control record](benchmarks/results/native-candidate-metadata-20260909-04-metadata-control-repeat.json)
is retained alongside all earlier records.

The controls differ by 833.63 TPS (1.38% of their mean); the candidates differ
by 1,156.67 TPS (1.87%). The mean candidate advantage is 1,757.41 TPS, or 2.92%,
and even the weaker candidate exceeds the stronger control by 1.26%. All three
predeclared numeric selection criteria pass. This is descriptive evidence from
two observations per setting, not a statistical significance result.

The consistent interior CPU sampling gives control values 9.3273 / 8.5409 and
candidate values 8.3295 / 8.7190 CPU equivalents: means **8.9341→8.5243 (−4.59%)**.
Mean rate-normalized CPU estimates are **148.45→137.56 µs/transfer (−7.34%)**.
These are not integrated CPU measurements or native-execution timings. The
control CPU range is 8.80% of its mean, and candidate 03 used more sampled CPU
than control 04; the mean saving is not a uniform reduction across all pairs.
Admission residence/backlog and dynamic block packing also varied. Metadata
projection does not establish that admission retries or delivery delays are fixed.

All configurations and immutable image identities match except for the intended
metadata flag; the VM remains 64 GiB / 24 vCPUs. The fourth arm required extra
preparation because host free disk fell to about 34 GiB. Before its controller
and client readiness/warmup began, unused Docker build cache was reclaimed
(31.39 GB reported), Docker's documented free-block reclamation command was run,
and approximately 11.3 GB of pip download cache was purged. Installed packages,
images and database volumes were preserved. Docker reclamation returned success
with unchanged genesis/Session Stats identities but did not increase host free
space here; pip cache cleanup raised it to about 45 GiB. No maintenance overlapped
load, and about 17 GiB remained after the last measured arm.

The [Docker Linux FAQ](https://docs.docker.com/desktop/troubleshoot-and-support/faqs/linuxfaqs/)
documents the reclamation command. Exact commands, completion timestamps, file
hashes, allocated-block measurements and before/after identities are embedded
in the saved evaluation. Its preparation-record timestamp is documentation time;
individual log completion times establish that maintenance preceded the final
control. Strict configuration fingerprints do not capture disk/cache/I/O state.
Consequently the four runs demonstrate **observed repeat consistency with
asymmetric maintenance**, not an uninterrupted identical-host experiment proving
that the flag alone caused the entire gain. No independent ≥5% offered overdrive
was present, so maximum capacity remains unestablished.

## Retained desktop configuration and next step

MyLocalTonDocker commit `91e4f70` selects the tested existing local images:

```dotenv
TON_BRANCH=admission-local-7b73cdb1
TON_IMAGE=mylocalton-ton
MLT_IMAGE=mylocalton-genesis
NATIVE_LOAD_IMAGE=mylocalton-client:admission-local-7b73cdb1
TON_NATIVE_CANDIDATE_METADATA_PROJECTION=1
TON_OVERLAY_LOCAL_SIGNATURE_REUSE=1
TON_NATIVE_ADMISSION_CONFIG_CACHE=1
TON_NATIVE_RECONCILIATION_PROFILE=1
TON_KEYRING_PREPARED_SIGNING=0
TON_NATIVE_ADMISSION_SHARD_SHARING=0
TON_NATIVE_ADMISSION_SNAPSHOT_REFRESH=0
```

All-profile Compose resolution equals the tested candidate configuration. The
four-lane database, quotas, source count, initial/maximum windows, 10 connections
and 20 ms coalescing are retained. Earlier winning images remain available;
there is no database reset. C++/Compose fallback and physical-server defaults
remain zero. These CPU-specific images and local commits are not a published
Server A release. On production, use a built revision containing the feature
and test it against its same-image control before changing the deployment preset.

The retained runtime was verified at 12:41 UTC: genesis and Session Stats were
healthy, masterchain sequence numbers advanced from 62,071 to
62,121, and the generator remained stopped after exit 0. Container
identities and frozen image IDs stayed unchanged during that check. Physical
Compose resolution still reports metadata projection 0. The saved evaluation
includes this runtime record. About 16.0 GiB of host disk space remains; arrange
additional free space before another full load cycle.

For two remote generators, use the
[disjoint-source procedure](native-remote-client-guide.md#two-remote-generators-against-one-genesis).
A second sender can increase unique offers and network traffic, including a
second whole-chain proof download. It only increases canonical TPS if the first
sender was limiting useful supply and the validator has remaining capacity.
Compare combined unique offers/admissions against one whole-chain canonical
count over a common window; never add the two followers' canonical TPS figures.
If offers increase while canonical TPS stays flat and RTT/backlog/retries rise,
profile the selected binary on A. Prioritize serialized admission/reconciliation
work if it still dominates, then measured collation/finalization and persistence
costs. More connections or a lane-owned redesign should follow that evidence.

Raw artifacts are retained under
`build/benchmarks/candidate-metadata-cycles-20260909/`.
