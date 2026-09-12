# Native lane pipeline experiments — 12 September 2026

The retained change from this series is the persistent producer: **55,891.51 →
57,135.33 mean canonical native-transfer TPS (+2.2254%)** in its four-arm ABBA
comparison. Both treatments exceeded both controls. Sampled validator CPU rose
2.263%, so this establishes neither improved CPU efficiency nor maximum capacity.
Preparation, the canonical journal, and independent owners/domains have **not**
earned promotion. The fresh balanced ownership comparison remains pending.

The earlier repeatable **61,959.20 TPS** metadata reference belongs to its own
comparison and maintenance history. The later **64,716.77 TPS** VM-volume screen
is a storage diagnostic with backpressure. Neither absolute rate can be combined
with this series to claim a feature gain or choose a winner across chains.
See the [historical reference](native-master-reference-2026-09-11.md) and the
[compact result record](benchmarks/results/native-lane-pipeline-20260912-summary.json).

## Completed comparisons

Every complete arm below measured 600 seconds with 20 ms coalescing, four lanes,
10 connections, 24,576 sources and a 65,536 maximum logical window. Canonical rates
use complete integer block-time seconds (599 seconds here); offered/admitted rates
use the 600-second offer window. Fresh stages have different chain histories.

| Producer stage2 order | Flag | Canonical TPS | Offered/admitted TPS |
|---|---:|---:|---:|
| A1 | 0 | 56,249.16 | 56,266.43 |
| B1 | 1 | 57,209.35 | 57,305.81 |
| B2 | 1 | 57,061.32 | 57,152.69 |
| A2 | 0 | 55,533.86 | 55,415.57 |

All four arms passed proof, drain, cleanup and strict image checks. Control drift
was −1.272%; both treatment repeats exceeded both controls. Retain producer1 for
subsequent tests; do not compound its +2.2254% with older comparisons.

Preparation stage3 was inconclusive. Its fresh-chain sequence completed B1 at
27,920.56 TPS, A1 at 25,210.58 and A2 at 27,961.72. Control drift was 10.913%; A2
exceeded B1. B2 stopped after a telemetry timeout and supplies no performance rate.
Preparation0 remains selected. A separate proof/cleanup attestation for copying
that settled chain did not turn the interrupted arm into a valid benchmark.

The same-chain VM-volume placement screen subsequently completed at 64,716.77
canonical TPS and 64,244.56 offered/admitted TPS. Its measured backpressure was
5.359%, with insufficient independent offered overdrive. Cold-copy history,
changing storage/runtime conditions and unrelated work prevent an isolated
storage-gain percentage. This screen did not promote preparation1.

| Journal stage4 order | Flag | Canonical TPS | Offered/admitted TPS |
|---|---:|---:|---:|
| A1 | 0 | 55,921.07 | 55,439.28 |
| B1 | 1 | 51,339.83 | 51,666.16 |
| B2 | 1 | 54,332.18 | 53,685.28 |
| A2 | 0 | 53,562.12 | 52,789.63 |

All four completed correctly. Journal mean was 52,836.01 versus 54,741.60 TPS for
controls (observed −3.481%). Control drift was −4.218%, and unrelated containers
changed during B1 and A2. No isolated causal regression is established, but there
is no repeatable gain: keep journal0. Roughly 92% fewer lookups per source visit
were useful work reductions, not evidence of a TPS improvement.

## Ownership evidence and invalid runs

The original stage1 control completed at 57,149.36 canonical TPS. The treatment's
old client misclassified owner generation races as congestion, and its cleanup
parser rejected topology booleans. Its diagnostic 27,722.71 TPS is excluded from
selection despite complete generator proof/drain. A separate reviewed cleanup
handoff permitted a corrected-client attempt; the original invalid verdict stayed.

The corrected control completed 600 seconds at diagnostic rates of **40,022.81
canonical / 40,173.79 offered-admitted TPS**, but failed the fixed lane-balance
gate: the last lane reached 9434 basis points of equal share, below 9500 required.
All lanes were active, totals reconciled, and proof/drain/cleanup passed. The
wrapper remained invalid and source reuse unauthorized; no tolerance was widened
and no corrected treatment was launched. There is no owner or client-effect
percentage from either context.

This control spent 65.521% of measurement under canonical backpressure. Blocks
averaged 18,066 native transfers but arrived at only 2.215 per second. The observed
18,432 ceiling is configured callback delivery, not a general protocol maximum.
Candidate mean native commit was 108.47 ms; external wait averaged 156.85 ms,
mostly first-work waiting; validation averaged 323.83 ms total. These scopes nest
and cannot be added as exclusive CPU costs. The actual CPU interval averaged
7.306 cores under quota16; VM-wide I/O full pressure was 9.25%, and unrelated
containers changed. No single cause, capacity, or client regression is isolated.

There were no measured window-cap or query-credit hits. Keep the 65,536 maximum
until ready work repeatedly waits there. Smaller callback delivery (for example,
8192) is a possible separately controlled experiment: shorter block turns may
trade against more setup/finalization. It is not a measured improvement.

## Selected benchmark profile

```dotenv
TON_NATIVE_PERSISTENT_PRODUCER=1
TON_NATIVE_ADMISSION_PREPARE=0
TON_NATIVE_CANONICAL_JOURNAL=0
TON_NATIVE_ADMISSION_LANE_OWNERS=0
TON_NATIVE_LANE_SCHEDULERS=0
TON_NATIVE_ADMISSION_SNAPSHOT_REFRESH=0
TON_NATIVE_CHECKPOINT_RETAIN_INGRESS=0
```

The [reference environment fragment](benchmark-profiles/native-lane-pipeline-reference.env)
records these flags and the matched client workload. Merge it only into a compatible
setup with matching four-lane wallets and published binaries containing the changes;
it selects no remote image tag and must not overwrite a live `.env`.

This is a selected desktop benchmark profile, not changed C++ defaults or a proved
48-core production optimum. Preserve the existing retained metadata/config-cache
and exact local-signature settings. Deployment still requires matching published
binaries and updated companion/client images; this report publishes no image.

## Validation and next context

Measured engine `52a20df0` passed 144 focused cases, 21 publication-gate invocations
and seven frozen-runtime target checks. Split-startup engine `095bc0de` repeated
those gates and the existing mock-manager ordinary-idle regression. The seven
frozen-runtime targets repeat gate cases; these counts are not additive distinct
tests. The mock test is not real Collator split verification.

The new source allows already validated before-split/after-split candidates to
seal without waiting for native ingress. Ordinary idle suppression, exact
predecessor/state/time checks, cancellation and forbidden-new-message checks remain;
after-merge is deliberately excluded. Real fresh zero-ingress root→2→4 verification
and the new throughput comparison are pending. Queued-parent behavior was reviewed
from unchanged teardown code, not newly proved by that mock test.

The corrected client `bc15f976` recognizes only two exact NTRN code-651 state-race
responses as bounded revision retries; unknown availability errors, actual pressure,
timeouts, retry horizons, exact parent bytes and proof accounting remain enforced.
It passed 66 policy cases and a production-worker/TL callback regression. The latter
uses synthetic parent bytes and is not cryptographic or performance validation.
MyLocalTonDocker `e04b96f` passed 53 parser/profiler cases; only allowlisted topology
fields accept boolean tokens. Required numeric cleanup gauges stay strict.

The new balanced context will use **16 engine workers** under the same16-CPU quota.
With domains enabled, allocation is main4 plus four groups of3; the earlier14-worker
allocation was main4 plus3/3/2/2. Client workers/signers remain **6/6** under6 CPUs;
wallet lane=index%4, so worker count does not assign two dedicated workers per lane.
Both future arms retain the same client, validator, sources, 20 ms coalescing,
32,768/65,536 windows, batch/query limits64 and signature/trie workers8.
Owners/domains0→4 change together; producer1, journal0 and preparation1 remain
fixed. Preparation1 is an owner dependency, not an independent promotion.

**Balanced A1/B1/B2/A2: pending.** Startup is paused because physical available
memory oscillated around the unchanged24 GiB preflight floor. No balanced validator
or load measurement has started; this is a resource/startup prerequisite, not a
completed performance test. A conditional owner win would still require a
complete-profile comparison against the selected preparation0 reference. Old
invalid or different-chain arms cannot substitute for the new controls.

## Follow-up experiments after the ownership comparison

1. Compare callback delivery limits of 18,432 and 8,192 with the client window
   unchanged. Smaller blocks must improve canonical TPS, not just block frequency;
   also compare commit/validation latency and setup cost per transfer.
2. Separate cell-store preparation CPU from durable-write waiting. CellDb holds
   its store operation until commit completes, and RocksDB uses synchronous writes.
   Measure write CPU, off-CPU time and queue delay before considering bounded I/O
   offload or group commit. Any implementation must acknowledge only durable data.
3. Compare four versus eight validation signature workers before introducing a
   reusable executor. Current verification creates and joins threads per batch;
   overlapping launch/join wall scopes alone do not establish thread-creation CPU
   cost. Require lower replay latency and improved canonical TPS with full checks.

These are hypotheses from the saved diagnostics, including an invalid control.
They are not performance gains or a forecast of 200,000 TPS. Additional connections
and larger windows remain lower priority while their existing credit goes unused.
