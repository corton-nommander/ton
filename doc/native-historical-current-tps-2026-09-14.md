# Actual historical build versus current build — 14 September 2026

The preserved **61k validator binary sustained 38,111.68 canonical TPS** in the
new common setup. The current build produced **42,458.28 TPS with rejected lane
balance**, followed by an accepted **37,434.10 TPS**. The final historical repeat
was interrupted by the host-memory guard after 552.68 of 600 measurement seconds.
**The planned ABBA comparison is incomplete and establishes no accepted performance
gain or regression.**

| Planned order | Build | Offered TPS | Canonical outputs / seconds | Canonical TPS | Result |
| --- | --- | ---: | ---: | ---: | --- |
| A1 | Historical `7b73cdb1` | 38,458.67 | 22,828,896 / 599 | **38,111.68** | Accepted |
| B1 | Current `925ee072` | 42,816.85 | 25,432,512 / 599 | **42,458.28** | Rejected: lane balance |
| B2 | Current `925ee072` | 37,514.72 | 22,423,024 / 599 | **37,434.10** | Accepted |
| A2 | Historical `7b73cdb1` | — | Incomplete | — | Host-memory guard stopped the run |

The single accepted current observation is **1.78% below** the single accepted
historical observation. Keeping both completed current observations, including the
rejected one, gives a descriptive mean of **39,946.19 TPS**, 4.81% above A1. Neither
calculation is an accepted ABBA result: the former has one selected passing sample
per build, and the latter contains a rejected sample and unequal sample counts.
A2 is excluded from complete-run arithmetic because it never finished. Its partial
summary still divides accumulated outputs by the planned 599-second bucket; that
number must not be presented as a completed ten-minute throughput measurement.

B1's four lane shares were **25.8041%, 26.9056%, 24.4953%, and 22.7951%**. The
frozen requirement is 23.75–26.25% for each lane, or ±5% relative to an equal
25% share. B1 passed proof and completion checks but failed this requirement.
Its failure remains recorded, with no accepted arm verdict. The separately frozen
continuation ran only the already-planned B2 and A2; it did not replace B1 or
relax its gate. Both initial one-minute compatibility smoke tests passed.

The earlier [historical reference](native-master-reference-2026-09-11.md) averaged
**61,959.20 TPS** from 61,380.86 and 62,537.54 TPS. A1, running the actual historical
binary, is **38.49% below that reference**. Reverting the validator build did not
restore 61k under the new common setup. Consequently, changes to the current
validator alone cannot explain this absolute difference. This does not exonerate
every intervening change: the common generator, seed state, packaging, host load
and wall-clock conditions differ from the original reference, and the matched
comparison remains incomplete.

Every completed sustained observation passed canonical proof, completion,
signed-run quantum, batching, image identity and validator cleanup checks.
Their windows contain **70,684,432 proof-checked native outputs**. Final backlog,
nonce gaps, hash conflicts, fatal follower errors, exhausted retries and rejected
basechain candidates were zero. Admission retries and timeouts did occur and
recovered; they are not lost transfers. The interrupted A2 has no final generator
record or final completion proof and cannot inherit these assertions.

| Diagnostic | A1 historical | B1 current, rejected | B2 current |
| --- | ---: | ---: | ---: |
| Measured backpressure fraction | 66.47% | 63.33% | 72.81% |
| Sampled canonical backlog peak | 2,097,120 | 2,097,120 | 2,097,120 |
| Full-run physical parent submission attempts | 1,936,332 | 2,239,823 | 1,873,248 |
| Snapshot-change retry results | 192,651 | 295,520 | 147,272 |
| Timeout results | 23 | 19 | 1,024 |
| Validator mean CPU cores | 6.07 | 6.40 | 6.13 |
| Validator sampled peak memory | 19.00 GiB | 19.56 GiB | 18.91 GiB |
| Host sampled minimum available RAM | 30.65 GiB | 29.02 GiB | 17.89 GiB |

Backpressure is the maximum accumulated worker pause time divided by measurement
duration, not the fraction during which all workers stopped. The load generator’s observed canonical backlog
repeatedly reached its limit. Every completed sustained arm failed the
capacity requirements for useful offered headroom and backpressure. These are
observed throughput results, not demonstrated maximum capacity. Candidate stage
wall timers overlap and do not establish exclusive CPU cost or a causal bottleneck.
Historical validation output-normalized telemetry is unavailable, not zero.

Docker CPU and memory values use snapshots within each measurement window, with
277/284/284 samples. The separate host observer covers approximately 590 of each
600 seconds using only fully contained recorded intervals. Whole-host CPU was
65.63%/67.14%/65.36%; CPU pressure was 21.76%/21.42%/22.66%. Paging occurred in all
three windows. Unrelated workloads remained active, and some unrelated containers
were recreated during A1. These observations limit attribution; they do not prove
that one background application caused the throughput or lane imbalance.
Host-PID based cgroup, thread and device attribution is invalid under this Docker
Desktop topology and is excluded.

The frozen guard stopped A2 at **12:32:58 UTC** after two samples below **16 GiB
host available RAM**. Availability at the trip was **15.994 GiB**, with essentially
all 8 GiB host swap used. The Docker VM still reported about 56 GiB available,
which is not additional physical-host reserve. Host disk had about 381 GiB free.
Host availability continued falling after shutdown, so the threshold was not
lowered to force a result. The generator exceeded its stop timeout and exited
**137**, with Docker reporting `OOMKilled=false`; the validator and stats service
exited 143. All benchmark runtime containers are stopped. Images, stopped seed,
per-arm volumes and raw evidence are retained. Completing the missing repeat
requires restored host-memory headroom and a separately recorded retry under the
original resource guard; the rejected B1 must remain in the record.

Both validators used **14 threads**, a 16-CPU quota and fixed CPU placement,
in-memory CellDb with a 16 GiB cache, and named-volume storage. The generator used
six CPUs, six workers/signers, ten connections, 24,576 source accounts and paired
destinations, four lanes, 16-output signed runs, batching of 64 and 20 ms coalescing.
Unpaced bounded load used 60 seconds warmup, 600 seconds measurement and up to
180 seconds drain. Canonical TPS counts logical native outputs over fully contained
integer block-time seconds; it can include warmup-offered work and differs from
offered-cohort TPS. This local native workload does not establish public-network
or general smart-contract throughput.

Metadata projection, overlay signature reuse, admission configuration caching and
reconciliation profiling were enabled in both builds. Persistent producer was
OFF for historical and ON for current, as selected in their respective build
profiles. Rooted execution and other experimental paths remained OFF. The
comparison therefore includes the selected build profiles and their runtime
libraries; it does not isolate one optimization.

Each arm received a byte- and metadata-verified clone of the same stopped seed:
33,402,800,611 database bytes plus identical wallets, shared files and stats
database. Source, target and source recheck inventories matched. WAL and SST files
were preserved; a graceful database close is not claimed. Each validator recovered
afresh and passed chain freshness checks before warmup. Identical starting bytes
do not make subsequent block heads, election phase or OS caches identical.

The historical source is `7b73cdb157a1f5e621e420e1e5c913726b267cf5` and its validator
SHA256 is `457e3b1b1005fa5638a48ddb5bdd07c9c66d956f4a45b1514b39feb50ae35287`.
All eleven preserved files—validator, historical generator and nine private runtime
libraries—match the original image receipt. The validator and libraries were
repackaged without recompilation; the deleted original Docker image was not
recovered. Both measured images used identical current startup scripts and init
behavior, and the **same current generator** served every arm. Current validator
source is `925ee0721c5b691debe3a2e37dba83d3daaae581`; companion harness source is
`e77de07689323e2a2439d24ddbd76fe2a878a1e1`. Source and image identities were frozen
before measurement. The stats helper has an exact image ID but lacks a source
revision label, so the broader reproducibility flag remains false.

An earlier preparation attempt exhausted its isolated 2 GiB copy-helper cgroup
with dirty file cache before any benchmark load. The replacement bounds writeback
and cache retention; its 5.28 GB probe peaked at 64.5 MiB with no OOM events.
The successful preparation copies and three complete measurement runs reported no
Docker OOM or unexpected restart. This is separate from the preparation OOM and
the guard-triggered forced stop of A2; it is not a claim that the entire task had
no memory failures.

The [machine-readable result](benchmarks/results/native-historical-current-tps-20260914.json)
records the incomplete status, exact rates, acceptance dimensions, image and
artifact hashes, resource evidence and interruption. Independent checks revalidated
frozen inputs, canonical arithmetic, lane shares, seed inventories and the original
gates. Raw evidence remains in
`build/benchmarks/historical-current-tps-7b73-vs-925ee-20260914T100023Z/`;
the failed preparation is retained separately in the sibling directory ending
`20260914T094224Z/`. No runtime code was changed for this report.
