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
TON_NATIVE_VALIDATION_SIGNATURE_CACHE_FASTPATH=0
TON_NATIVE_VALIDATION_SIGNATURE_PERSISTENT_POOL=0
TON_NATIVE_DIRECT_RUN_ZERO_COPY=0
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

1. Keep the validation signature executor default-off. Its same-image live sequence
   completed valid 600-second A1 and B1 arms: flag-off A1 reached
   55,654.19699499165 canonical TPS and flag-on B1 reached 55,089.33555926544,
   1.014948% below A1. The frozen promotion rule requires every treatment above
   every control, so this ordering makes promotion impossible. B2 then failed the
   inner `physical_host_memory_below_24_gib` preflight before the generator launched;
   A2 was not run. The incomplete sides are not averaged, and there is no capacity
   claim.

   The mechanism did work. A1 observed 5,000 helper calls, 2,373,081 parents,
   34,936 created threads, 126.291207 seconds of launch, 22.567329 seconds of join
   and 149.004354 seconds of executor residence. B1 observed 4,119 calls and
   2,349,492 parents; 3,641 pool calls submitted 29,128 tickets to eight persistent
   threads, with zero launch/join sums, 10.556876 seconds of residence, zero
   contended submits, peak active/queue gauges of 8/24 and final active/queue
   gauges of 0/0.
   The two arms have different call totals, so these counters are mechanism evidence,
   not a normalized efficiency gain. The exact decision and artifact hashes are in
   the [signature-pool result](benchmarks/results/native-signature-pool-20260913.json).
2. Collect the new Collator-to-Manager, Manager-to-pool and pool-to-first-epoch
   timestamps before changing callback order. The existing pre-epoch interval
   averaged 56.661/62.225 ms across A1/B1, while the ExtMessagePool actor type ran
   at 0.930228/0.903469 load and included five actors. If a pool boundary dominates,
   run a same-process 64/12/12/64 admission-query-credit comparison. Cap 12 gives
   120 aggregate credits, above observed admission demand but below the measured
   per-client maxima of 23/22. Require positive credit stalls and both treatment
   TPS values above both controls; stop on lower offered load or unchanged stage tails.
3. Run the default-off CellDb durability attribution in three same-image arms:
   profile off with sync true, profile on with sync true, then profile on with the
   explicitly unsafe sync-false ceiling on a disposable database. The saved A1/B1
   `celldb_store_cell` scopes averaged 39.23/58.25 ms per call, and the single CellDb
   actor's maximum turns reached 1.721064/1.305125 seconds. If sync false does not
   move canonical TPS and commit tails, stop. If it does, prototype a bounded cohort
   commit that resolves promises only after durability.
4. Run a fresh 18,432-versus-8,192 callback-limit ABBA with owners/domains/preparation
   off and producer on. Keep the physical transport window at 1,024 and client window
   65,536 fixed. The smaller cap needs to improve canonical TPS as well as cadence;
   use 12,288 only if 8,192 improves tails but loses throughput.
5. Publish one immutable, complete canonical state epoch and an application-time
   account change feed. This must remove hot-path state waits, full tracked-source
   walks and replacement-vector publication; the failed post-hoc journal already
   showed that fewer lookups alone do not guarantee more TPS.
6. Move pure collation and validation work to a lane-aware global work-stealing
   arena while keeping mutation and root publication ordered per lane. Aggregate
   CPU headroom is consistent with underused fixed scheduler islands, but per-group
   utilization was not measured. A separate 2×2
   owners/domains test can identify which switch caused that loss, but it is a
   diagnostic rather than the leading scaling path.
7. Before reusing locally decoded candidate data, add an exclusive timer around
   the validator's second BOC deserialize. All A1/B1 validations matched local
   candidates, and native unpack averaged 20.628/25.158 ms wall, but the whole
   stage consumed only about 0.148 CPU core. If the exclusive decode is material,
   hand off only an immutable root under exact block/file/root identity checks and
   fall back on every mismatch. Do not retain decoded batches or reuse replay
   results across the independent validator boundary.

These are hypotheses from the saved diagnostics, including the valid balanced
comparison. They are not performance gains or a forecast of 200,000 TPS. Additional
connections, RAM and larger windows remain lower priority given whole-container
CPU headroom and unused window/query credit; per-group saturation remains possible.

## September 14 continuation

The exact positive signature-cache preflight has an independent `0/1` control,
but its resumed same-image ABBA comparison was interrupted. Keep
`TON_NATIVE_VALIDATION_SIGNATURE_CACHE_FASTPATH=0`; it is now also the unset C++
default. The [cache-preflight record](benchmarks/results/native-signature-cache-fastpath-20260914.json)
preserves the observations and validity limitations. A1 is the sole accepted arm,
at 49,855.33 canonical TPS. B1's recovered 41,440.05 TPS is diagnostic only: its
wrapper exited 141 after the final JSON record was split by stderr at a Docker
log-frame boundary. B2 and A2 did not run. Neither the recovered final record nor
its active cache-hit counters turn B1 into an accepted benchmark. There is no
completed-ABBA mean, treatment speedup, promotion or capacity claim from that context.

A larger work reduction is now implemented behind
`TON_NATIVE_DIRECT_RUN_ZERO_COPY=1`: collation and validation retain canonical
v5/v6 signed parents, avoid the expanded logical-entry vector and reuse canonical
parent roots during serialization. One transfer scratch per parent feeds the
same state engine in wire order after parent authorization. Version, domain,
locality and canonical-tree checks remain enforced. The flag defaults off.

The [rooted-run design and validation](native-rooted-runs.md) document 13 passing
metadata tests and 40 passing cell tests, including differential wire and state
replay checks. The [compact result](benchmarks/results/native-rooted-runs-20260914.json)
records local microbenchmark evidence. An actual native Collator
runtime check is still required; these tests and codec timings establish no live
TPS gain.

Benchmark reporting was corrected separately:

- Payment-lane preflight reads the resolved Compose environment instead of an
  unsourced host variable (`245a337`).
- Final-record extraction from the original merged Docker log requires exactly
  one complete record, records the source hash and offsets, and rejects malformed
  or incomplete input (`5884fe`). The generator now also emits terminal stderr
  messages before its final stdout JSON record, avoiding its own shutdown-message
  insertion between Docker log fragments.
- The indexed report reducer (`0aa77c9`, `e67028e`) processed the archived
  163,008,240-byte A1 input in 24.604 seconds at nice 10 and reproduced the original
  summary byte for byte, SHA-256
  `4cb34de2137eb26a71921004c80b90bf2cf781bf07517c8c08d9a2b99e61b127`.
  The earlier approximately 16-minute reduction is a historical observation;
  the baseline was not rerun as a controlled timing pair. The equivalence receipt
  is linked from the cache-preflight result. Exact byte equivalence applies to
  those parser revisions; later additive rooted-run report fields change the
  output schema.
- The entrypoint exec chain (`467e8b0`) passes fake-engine PID and SIGTERM tests.
  An image-level validator stop test has not been completed.

These repairs improve reporting and process control. They do not authorize
substituting an interrupted arm for a valid performance comparison.
