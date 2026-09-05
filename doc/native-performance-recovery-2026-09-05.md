# Native performance recovery, 2026-09-05

## Recovered stopping point

The repository resumed at `63285ce9bbfd95e7f430699e53f5ca74d1914f46`
(`perf: reuse canonical native runs during batch decode`). The working tree
already contained unfinished changes in:

- `validator/impl/collator.cpp`
- `validator/impl/collator-impl.h`
- `validator/interfaces/validator-manager.h`
- `test/validator/test-collator-external-wait-stats.cpp`

Those changes pass the canonical NTRN value decoded during external-message
registration into native collation, avoiding a second decode of the same
immutable root, and count successful reuse. They were preserved during recovery.
The earlier completed commits include exact-parent nonce floors (`315ff6fc`),
bounded native-work scratch (`fec28851`), checkpoint ownership (`2610b3ad`),
stable signed-run quantum (`9f813e03`), and opt-in dense packing (`6b7f04c4`).

The source implementation was therefore ahead of the requested task list, but
its early source/nonce gate and several verification requirements were unfinished.
The performance targets below must not be inferred from the existence of commits.

## Historical measurements and their limits

Artifacts are under the sibling checkout
`../MyLocalTonDocker/benchmark-results/`. Run identifiers below are directory
prefixes; the full directory names also record revision, target load, and duration.
`generator-summary.json` supplies proof-checked canonical throughput, offered
load, validity, and signed-run density. `validator-pipeline-summary.json` supplies
measured-window collation costs and accepted logical transfers.

### Exact-parent nonce floors

| Metric | `20260904T121032Z` / `c1cd23d5`, 300 s | `20260904T164647Z` / `315ff6fc`, 300 s |
| --- | ---: | ---: |
| Measured nonce-mismatch logical transfers | 1,776,090 | 44,560 |
| Measured accepted logical transfers | 8,186,748 | 9,055,680 |
| Scheduler excluded entries, full-run delta | 3,493,932 | 0 |
| Scheduler scanned entries, full-run delta | 4,840,913 | 1,214,428 |
| Offered TPS | 26,460.16 | 29,916.05 |
| Canonical TPS | 26,489.58 | 29,943.01 |

The recorded nonce-mismatch count fell 97.49%. Full-run scheduler exclusions fell
to zero and scanned entries fell 74.91%; these counters come from
`validator-pool-summary.json` and cover the run's before/after collection window,
not exactly the generator's measured window.

The earlier control is ingress- and chain-capacity-invalid: canonical
backpressure exceeded 1%, the requested offered load was not attained, and load
above canonical throughput was insufficient. The later run's offered/canonical
ratio was only 0.99910. These observations support the nonce-floor mechanism,
but do not establish a repeatable >=2% capacity/TPS improvement under strict,
adequately offered paired runs.

### Native-work scratch

Execution cost is `native_execute.avg * native_execute.samples / accepted`, using
`measured.collated_basechain` from each pipeline summary. It is real elapsed
native-execution time per accepted logical transfer, not end-to-end transaction
latency or isolated scratch-only cost.

| Run prefix / revision | Duration | Execute microseconds / accepted | Canonical TPS | Signed-run density |
| --- | ---: | ---: | ---: | ---: |
| `20260904T164647Z` / `315ff6fc` control | 300 s | 1.93009 | 29,943.01 | 16.00 |
| `20260904T170441Z` / `fec28851` canary | 60 s | 1.99104 | 29,618.44 | 16.00 |
| `20260904T170853Z` / `fec28851` long run | 300 s | 3.07796 | 21,845.73 | 7.51 |
| `20260904T172204Z` / `fec28851` repeat | 60 s | 1.99362 | 29,867.39 | 16.00 |

The two dense canaries were about 3.16% and 3.29% slower in this stage than the
300-second control; their TPS changes were -1.08% and -0.25%. Different durations
and offered-load ceilings prevent treating them as conclusive paired capacity
measurements. They do not demonstrate the requested >=15% execution reduction or
>=1% TPS gain.

The long scratch run is explicitly capacity-invalid: it offered only 21,921.55
TPS against the 30,000 target, and its average logical transfers per signed run
fell from 16 to 7.51. Its apparent regression is confounded by changed load shape.
The new early source gate is included in the recovery diagnostic described below;
no valid repeated capacity A/B has been established.

### Invalid image/restart comparisons and retention

The ingress control `20260904T112103Z` / `bb2e8a8d` is also capacity-invalid:
signed-run density was 5.89, offered throughput was 17,399.41 TPS, and canonical
throughput was 17,283.05 TPS. Together with the long scratch run above, it is a
concrete example of a density-changed measurement that cannot support an A/B
capacity conclusion. The resumed task identifies derived-image rebuilds and
validator restarts as the source of two corrupted controls; future measurements
must record image identities and unchanged validator start times during each run.
A zero Docker restart count alone does not prove continuity after container
replacement.

Broad ingress-checkpoint retention remains default-off after its earlier
regression. Recovery tightens its optional path to require actual pending producer
work, in addition to the existing bounded ingress/deadline/capacity checks. No
new retention performance claim is made.

## Recovery changes

- Exact-parent floor propagation and callback-local scheduling are retained.
  Additional regression coverage checks competing forks, canonical watermark
  precedence, and source isolation. Floors remain callback-local and never
  advance the shared canonical watermark.
- Per-signed-work `std::set`/`std::map` scratch was already replaced in `fec28851`
  with `NativeWorkScratch`, a sorted fixed-capacity `std::array` of addresses and
  optional snapshots. The capacity is 32 endpoints for the protocol maximum of
  16 outputs. This fulfills the heap-free scratch objective with inline bounded
  storage; converting it to reserved vectors would add heap ownership without
  removing a remaining per-run allocation. Fragment and checkpoint journals
  still use maps/sets and are outside the per-signed-work scratch replacement.
- Native collation now loads and checks the first source's availability, balance
  representation, status, and nonce before collecting endpoints or loading any
  destination. A failed gate defers the whole signed identity and its logical
  transfer count. Passing the gate only permits normal execution: full-work size
  reservation, per-transfer validation, journaling, and atomic rollback remain.
- The pure `check_native_transfer_source` helper shares status/nonce rules with
  `execute_native_transfer_state`. Full execution preserves its existing error
  ordering. Expired work bypasses early preflight so its previous permanent
  rejection handling is retained. Tests exercise native/non-native account
  statuses, stale/future/maximal nonces, rejection precedence, later balance
  errors, and self-transfers. For unexpired work with multiple failures, early
  preflight can report a source/nonce deferral before a destination failure; it
  does not change whether that work can be accepted.
- Production-size histograms record unique accounts per execution microbatch and
  account updates per staged-trie build, including selected worker counts. The
  production threshold remains 512 pending representative production histograms
  and end-to-end evidence.
- Ingress retention additionally requires producer work to be selected ahead;
  merely having an open producer epoch does not retain an empty ingress wait.
- The branch-image workflow records immutable image identities for reproducible
  prebuilt-image use. Future measurements must use the strict protocol below.

## New verification

The Release/native Clang 22 build completed successfully with four build workers,
including `validator-engine` and `bench-staged-trie`. The focused CTest run passed
all nine targets: scheduler, scratch, histogram serialization, resolver policy,
finalization metadata, and four consensus pacing/idle/cancellation configurations.
`test-cells --filter Native` passed 18 tests; a separate
`test-cells --filter AugmentedDictionary` passed six tests. Multiple filters in
this runner intersect, so the initial combined filter selected zero tests and
was replaced by those two independently verified invocations.

The scheduler's added exact-parent regression delivers the identical eight-message
suffix while reducing 32 excluded ancestor scans to zero, then verifies that a
competing same-source callback can still select nonces 1–3 and that the global
watermark remains zero. This is a deterministic mechanism check, not a capacity
measurement.

The final sibling harness reporting suite also passed after independent review.
Strict identity includes both the Docker container tuple and the actual daemon's
PID/kernel start ticks; daemon restarts inside a surviving container invalidate
the run. Histogram parsing rejects missing, malformed and duplicate tokens.
Capacity and below-capacity load validation share correctness, completion,
signed-run quantum, ingress, lane, cleanup and required image-stability gates.
Only the former permits a capacity claim, and it additionally requires actual
measured offered TPS above canonical TPS.

Build/test logs are retained locally under `build/benchmarks/recovery-20260905/`.
The branch-image workflow passed local YAML parsing and assertions for SHA tags,
revision build arguments, and exact per-platform digest merging. The workflow
has not been published or executed in CI during this recovery.

## Staged-trie worker-tier benchmark

Completed two independent 5-round, 5-iteration matrices with the same compiled
binary: seven update sizes (64, 80, 128, 192, 256, 384, 512), four worker budgets,
uniform/prefix2/prefix16 addresses, and plain/tracked prior state. All 8,400 timed
results matched sequential roots. Genesis provisioning was paused during both
matrices; no builds or load generator ran concurrently. The runs took 12.05 and
12.07 seconds. Raw CSVs, binary/host provenance, methodology and paired results
are in [the worker benchmark report](benchmarks/staged-trie.md).

For the production-like prefix2 tracked case at 80 updates, wall medians for
1/2/4/8 workers were 991.8/837.0/849.9/972.8 microseconds. Two workers gave about
1.18x paired speedup. The prefix2 plain case at the same size favored one worker:
910.0/1855.7/1732.0/1942.2 microseconds. This dependence on prior-state tracking
prevents a global threshold decision from these measurements alone. The
production threshold remains 512; the new histograms must first show the actual
staged-checkpoint population. These timings are not validator TPS measurements.

## Recovered live environment and diagnostic protocol

The reboot left Docker images available but no local containers or data volumes.
A fresh local depth-2 network was provisioned from the existing control images,
including 24,576 sources and 49,152 wallets. The idle work-driven chain missed
scheduled shard-split windows during readiness. An unmeasured attempt was stopped
and preserved as `recovery-20260905-control1-readiness-stall`; temporary paced
production completed the four required lanes. Work-driven mode was restored
before every measured run. This is a preparation issue observed on the existing
control, not evidence of a treatment regression or a proven permanent deadlock.

The control is revision `63285ce9bbfd95e7f430699e53f5ca74d1914f46`. The recovery
image was built once with four build workers and tagged
`resume-sourcegate-3613439ca927`. Its recorded dirty-source SHA-256 is
`3613439ca927b6f85c973bb5e97d735b16ec02a4198c86fb31054cf6b72b258c`;
its OCI revision is the control revision suffixed with `-dirty-3613439ca927`.
Both derived validator and generator images were also prebuilt before the run
sequence. No image was rebuilt or pulled during a measured run. Retention stayed
off and the staged-trie threshold stayed 512.

Each screening run used 30 seconds of ramp, 60 seconds of warm-up, 60 seconds of
measurement and up to 180 seconds of drain. Control and treatment use the same
four-lane profile and normal signed-run quantum of 16. The recovery treatment
combines registration decode reuse, the early source gate and new telemetry;
it does not isolate either historical nonce-floor or fixed-scratch commits.

The first 40k control exposed a strict-reuse harness bug: Compose's image listing
included service dependencies, so selecting the last image compared the generator
to the genesis image. The raw run remains rejected. Actual validator and generator
identities were stable, but the result was also independently capacity-ineligible:
actual offered TPS was 37,454.93 (93.64% of target), below the existing 95% target
attainment requirement. The resolver now selects exactly
`.services[$service].image` from Compose JSON and tests reject missing/empty image
values and dependency-order mistakes. No acceptance threshold was weakened.

The next control used a 39k target. Its strict identity, proof, completion,
signed-run quantum, lane and cleanup checks passed. Actual offered TPS fell to
32,647.20, so ingress/chain capacity checks failed target attainment. The planned
repeated A/B sequence was stopped; one matched treatment was run for correctness,
telemetry and execution-cost diagnostics only. Actual offer above canonical rate
is necessary but does not excuse failed existing ingress gates.

### Completed 39k screening results

| Metric | Control | Recovery treatment |
| --- | ---: | ---: |
| Actual measured offered TPS | 32,647.20 | 35,144.00 |
| Proof-checked canonical chain-window TPS | 32,032.81 | 35,284.61 |
| Normal signed-run density | 16.00 | 16.00 |
| Measured accepted candidate transfers | 1,958,224 | 2,113,984 |
| Native execute microseconds / accepted | 1.56977 | 1.62590 |
| Measured nonce-mismatch transfers | 0 | 0 |
| Scheduler excluded entries, full-run delta | 0 | 0 |
| Strict image/container/daemon continuity | Pass | Pass |
| Proof, completion, quantum, lanes, cleanup | Pass | Pass |
| Ingress/chain capacity eligibility | Fail | Fail |

Treatment execution cost was descriptively 3.58% higher per accepted transfer.
Canonical TPS was descriptively 10.15% higher while actual offered TPS was 7.65%
higher. These rejected single runs do not establish a speedup or a repeatable
regression: target attainment failed in both, and treatment offered load was also
below canonical throughput. The >=15% execution reduction and >=1%/>=2% TPS
objectives remain unproven. The already-zero nonce and exclusion controls cannot
establish another percentage reduction, and both images already contain the earlier
floor and scratch implementations.

Treatment recorded 132,124 registration-decoded run reuses for 2,113,984 logical
transfers (16 per run), with no measured native deferrals, permanent rejections or
checkpoint rollbacks. Its histograms were complete and reconciled across 681
candidate records: all 8,909 execution microbatches had <=64 unique accounts.
Staged builds had 7,249 / 26 / 87 / 379 attempts in the <=64 / 65-80 / 81-128 /
129-256 update buckets. No staged build reached 257 updates; all 7,741 selected
one worker. Thus checkpoint aggregation does exceed the microbatch population,
but the 512 threshold remained dormant in this measured workload. The exact
staged maximum within its bucket and plain/tracked topology mix are not measured.

Both runs also retain `reproducible=false`: the harness source tree was dirty and
the unchanged session-stats image lacked a source-revision label. This flag was
not rewritten. Immutable validator/generator identities and the archived treatment
source diff improve provenance but do not override the harness's independent
reproducibility result.

Completed bundles are
`../MyLocalTonDocker/benchmark-results/recovery-20260905-control1-39k-60s/` and
`../MyLocalTonDocker/benchmark-results/recovery-20260905-treatment1-39k-60s/`.
A compact [screening evidence extract](benchmarks/results/native-recovery-screening-20260905.json)
preserves summary hashes, acceptance reasons, image identities, rates and counters.
The detailed local extract is `build/benchmarks/recovery-20260905/ab-extraction-final.json`.
The planned second treatment and bracket control were not run after capacity
eligibility failed. After artifact collection, the local validator was restored
to the prebuilt control with chain volumes preserved; the treatment images remain
available. No source commit, registry publication or CI execution was performed.

## Strict A/B measurement protocol

1. Build each intended control/treatment image once, outside the measurement
   sequence. Record source revision, dirty diff hash, OCI revision, immutable
   manifest digest, and local image ID. Pin derived validator and generator
   images too; changing only the base tag is insufficient.
2. Use prebuilt strict reuse for every paired run. Do not rebuild or pull mutable
   tags as part of measurement preparation. Verify the selected image identity
   before load starts and after collection finishes.
3. Start the intended validator image before warm-up. Record container ID,
   image ID, process/container start time, and restart count. Abort that run's
   capacity comparison if its validator restarts or is replaced during the run.
   Switching images between control and treatment is a deliberate setup step,
   followed by the same warm-up procedure for each image.
4. Keep run duration, source set, source nonce discovery, signed-run target
   quantum, generator concurrency, validator CPU allocation, and all unrelated
   policy settings identical. Check actual signed-run density; a target of 16
   does not establish that the generator delivered 16.
5. Offer sustained load above observed canonical throughput, and require the
   harness's ingress, correctness, completion, lane-balance, and chain-capacity
   validity checks to pass. A throughput plateau at the offered rate is evidence
   about that load level, not maximum capacity.
6. Repeat paired controls/treatments with bracket controls. Compare native
   execution time per accepted logical transfer, mismatch/exclusion counters
   normalized to clearly stated windows, and proof-checked canonical throughput.
   Report repeat variability and reject density/restart/load-confounded runs
   before judging the >=90%, >50%, >=2%, >=15%, and >=1% targets.
