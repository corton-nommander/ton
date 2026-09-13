# Native lane pipeline experiments — 12 September 2026

The retained change from this series is the persistent producer: **55,891.51 →
57,135.33 mean canonical native-transfer TPS (+2.2254%)** in its four-arm ABBA
comparison. Both treatments exceeded both controls. Sampled validator CPU rose
2.263%, so this establishes neither improved CPU efficiency nor maximum capacity.
Preparation and the canonical journal have **not** earned promotion. The resumed
balanced comparison completed on 13 September: owners = 4 plus scheduler domains = 4
averaged **52,476.47 TPS** against **57,680.72** controls (**−9.0225%**). Reject
that combined configuration; the selected owners = 0 / domains = 0 profile is unchanged.

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

## Ownership evidence, balanced result and invalid runs

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

After more RAM became available, a new fresh context completed the required
600-second A1/B1/B2/A2 comparison with 16 engine workers under 16 CPUs. Preparation = 1
was fixed in every arm because it is an owner dependency; producer = 1 and journal = 0
were also fixed. The treatment changed owners and scheduler domains together from
0/0 to 4/4.

| Balanced stage1 order | Owners/domains | Canonical TPS | Offered/admitted TPS | Native blocks/s | Transfers/block |
|---|---:|---:|---:|---:|---:|
| A1 | 0/0 | 59,149.38 | 58,047.71 | 6.290 | 9,402.99 |
| B1 | 4/4 | 52,410.20 | 52,181.97 | 3.250 | 16,124.15 |
| B2 | 4/4 | 52,542.74 | 52,514.59 | 3.639 | 14,437.20 |
| A2 | 0/0 | 56,212.06 | 55,181.76 | 4.656 | 12,072.79 |

All four arms passed the frozen image, 600-second, lane-balance, proof, drain,
cleanup and chronological source-handoff checks. Control drift was −4.966% and
treatment drift was +0.253%. Treatment density rose 42.306%, but native-block
cadence fell 37.060%; both treatment repeats were below both controls. The valid
comparison therefore rejects the combined configuration without attributing the
loss separately to ownership or scheduler placement. No stage 1 arm is promoted:
its controls contain preparation = 1 and are diagnostic, while the selected complete
profile retains preparation = 0.

The workload did not establish capacity. Offered rates were below canonical rates
in the controls and supplied no independent 5% surplus. Sampled validator CPU was
also lower in the treatments (6.53/6.86 core equivalents) than in the controls
(8.42/9.84), so the treatments showed lower whole-container activity while losing
throughput. That does not rule out saturation in one scheduler group or a
serialized actor, and no RAM or CPU-quota capacity conclusion follows.

This control spent 65.521% of measurement under canonical backpressure. Blocks
averaged 18,066 native transfers but arrived at only 2.215 per second. The observed
18,432 ceiling is configured callback delivery, not a general protocol maximum.
Candidate mean native commit was 108.47 ms; external wait averaged 156.85 ms,
mostly first-work waiting; validation averaged 323.83 ms total. These scopes nest
and cannot be added as exclusive CPU costs. The actual CPU interval averaged
7.306 cores under a 16-core quota; VM-wide I/O full pressure was 9.25%, and unrelated
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
after-merge is deliberately excluded. The fresh startup reached four reported
depth-two leaves before the load generator ran; two samples advanced masterchain
sequence 1374→1395 while all 28 native/live counters remained zero. This is a
reported-topology observation, not an independent proof of split ancestry. Queued
parent behavior was reviewed from unchanged teardown code, not newly proved by
that observation.

The corrected client `bc15f976` recognizes only two exact NTRN code-651 state-race
responses as bounded revision retries; unknown availability errors, actual pressure,
timeouts, retry horizons, exact parent bytes and proof accounting remain enforced.
It passed 66 policy cases and a production-worker/TL callback regression. The latter
uses synthetic parent bytes and is not cryptographic or performance validation.
MyLocalTonDocker `e04b96f` passed 53 parser/profiler cases; only allowlisted topology
fields accept boolean tokens. Required numeric cleanup gauges stay strict.

The completed balanced context used **16 engine workers** under the same 16-CPU
quota. Enabled domains allocated four main workers plus four groups of three. Client
workers/signers remained **6/6** under 6 CPUs; wallet lane = index % 4. Every arm
retained the same images, sources, 20 ms coalescing, 32,768/65,536 windows,
batch/query limits of 64 and eight signature/trie workers. The final frozen
comparison and explicit rejection are recorded in the compact result record
linked above. Old invalid or different-chain arms were not substituted for its
controls.

## Follow-up experiments after the ownership comparison

1. The new default-off prototype replaces per-validation signature thread creation
   with one persistent bounded executor, without changing replay order. A1 recorded
   3,607 helper calls, 26,280 created threads and 82.16 seconds in launch scopes;
   validation replay averaged 51.70 ms inside 113.15 ms total validation. Its unit
   gate covers concurrent callers, queue backpressure, worker reuse and deterministic
   rejection indices. A 256-run fresh-process corpus now fixes and matches the exact
   batch root, serialized BOC digest and replay-state digest in flag-off and flag-on
   processes. Next run a same-image flag-off/flag-on ABBA.
2. Run a fresh 18,432-versus-8,192 callback-limit ABBA with owners/domains/preparation
   off and producer on. Keep the physical transport window at 1,024 and client window
   65,536 fixed. The smaller cap needs to improve canonical TPS as well as cadence;
   use 12,288 only if 8,192 improves tails but loses throughput.
3. Split CellDb preparation from synchronous durable-write waiting. Instrument
   enqueue-to-actor delay, commit wall/thread CPU, `fsync`/`fdatasync`, RocksDB
   stalls and durable acknowledgements. A throwaway-chain `sync=false` run is only
   a ceiling diagnostic; if it does not move canonical TPS, stop. If it does,
   prototype a bounded cohort commit that resolves promises only after durability.
4. Publish one immutable, complete canonical state epoch and an application-time
   account change feed. This must remove hot-path state waits, full tracked-source
   walks and replacement-vector publication; the failed post-hoc journal already
   showed that fewer lookups alone do not guarantee more TPS.
5. Move pure collation and validation work to a lane-aware global work-stealing
   arena while keeping mutation and root publication ordered per lane. Aggregate
   CPU headroom is consistent with underused fixed scheduler islands, but per-group
   utilization was not measured. A separate 2×2
   owners/domains test can identify which switch caused that loss, but it is a
   diagnostic rather than the leading scaling path.

These are hypotheses from the saved diagnostics, including the valid balanced
comparison. They are not performance gains or a forecast of 200,000 TPS. Additional
connections, RAM and larger windows remain lower priority given whole-container
CPU headroom and unused window/query credit; per-group saturation remains possible.
