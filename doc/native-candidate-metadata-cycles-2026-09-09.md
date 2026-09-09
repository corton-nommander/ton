# Strict candidate metadata projection — 9 September 2026

Status: implementation, correctness checks and microbenchmark complete; matched
TPS measurements are running. The user approved
this next CPU target after the completed
[admission/signing cycle](native-admission-cycles-2026-09-09.md). No TPS gain is
assigned to this candidate before matched measurements complete.

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

`TON_NATIVE_CANDIDATE_METADATA_PROJECTION=1` selects the new path at process
startup; only literal `1` enables it, and the default is 0. An explicit internal
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
startup flag both off and on, preserving parent identity, deduplication, nonce
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
changes only metadata projection from 0 to 1. `.env.desktop` retains the previous
winning images until this comparison supports a new selection. The existing
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
is retained. The next arm changes only metadata projection to 1.

Raw artifacts are retained under
`build/benchmarks/candidate-metadata-cycles-20260909/`.
