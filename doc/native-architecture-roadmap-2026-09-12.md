# Native sidechain architecture priorities — 12 September 2026

The most credible route to 200,000 canonical native transfers/s is to partition
and overlap the complete lane pipeline. The retained reference is **61,959.20
canonical logical TPS** on runtime `7b73cdb1`, already in master ancestry. Reaching
200k from that reference requires **3.228×**. None of the completed experiments
establishes that multiplier.

These counts are logical native transfers, including the individual transfers
authorized by a signed NTRN parent. They are not external BOC/RPC counts, discovery
rates, or short chart peaks.

The repeated improvements worth retaining are local overlay signature reuse
(**57,673.19 → 60,039.59, +4.10%**) and strict parent metadata projection
(**60,201.79 → 61,959.20, +2.92%** in its own comparison). Do not compound separate
experiments into another measured result. The metadata comparison includes a
maintenance caveat before its last control and lacks independent offered-load
headroom. Its two candidate runs were 61,380.86 and 62,537.54 TPS. A later unchanged
control reached 65,831.13 after maintenance; that is not a treatment gain.
See the [retained reference](native-master-reference-2026-09-11.md) and
[metadata evidence](native-candidate-metadata-cycles-2026-09-09.md).

The lane-pipeline series subsequently retained the persistent producer at
**55,891.51 → 57,135.33 TPS (+2.2254%)**. Preparation was inconclusive and the
canonical journal reduced normalized lookups by roughly 92% without a repeatable
TPS gain. The fresh balanced A1/B1/B2/A2 comparison completed on 13 September:
combined owners = 4 / domains = 4 averaged **52,476.47 TPS** against **57,680.72** controls
(**−9.0225%**). Both treatment repeats lost to both controls, so the combined
configuration was rejected. Preparation = 1 was only a common owner dependency;
the selected profile remains producer = 1, preparation = 0, journal = 0,
owners = 0, domains = 0.

The balanced result changes the diagnosis. Candidate density increased 42.306%,
but native-block cadence fell 37.060%. In the first pair, the treatment used fewer
sampled validator cores (6.53 versus 8.42), while nonempty collation conversion
fell from roughly 61% to 32% and 70.7% of its blocks reached the 18,432 callback cap.
The samples show whole-container CPU headroom, no memory failure, unused client
windows and unused admission query credits. They do not rule out saturation in
one scheduler group or a serialized actor.
More RAM allowed the fresh experiment to start; it did not remove the throughput
limit. These runs also lacked independent 5% offered-load surplus and therefore do
not establish maximum capacity.

## Immediate controlled experiment

Before another source redesign, compare `TON_NATIVE_COLLATOR_QUEUE_LIMIT=18432`
against `8192` in a fresh ABBA using the selected owners = 0, domains = 0,
preparation = 0 profile. Keep the physical transport window at 1,024, client
window at 32,768/65,536, batch/query limit at 64, 24,576 sources and 60/600/180
timing fixed. Both caps align with the 512-transfer scheduler fragment and
16-transfer signed-run quantum, and require only
container recreation with the existing image. The smaller cap must improve
canonical TPS, not merely blocks/s or latency. Add 12,288 only if 8,192 improves
cadence and write tails but loses throughput.

## Ranked architecture directions

The following bounds are conditional latency or work envelopes from saved
diagnostics. They use different, overlapping denominators, cannot be multiplied,
and are not TPS forecasts.

1. **Keep the persistent executor default-off; isolate replay components before
   changing their scheduling.** The same-image live first pair reached
   55,654.19699499165 canonical TPS with the executor off and 55,089.33555926544
   with it on (−1.014948%). Because strict promotion requires every treatment arm
   above every control arm, B1 below A1 makes promotion impossible. B2 was excluded
   before generator launch by the inner physical-memory preflight, so there is no
   completed-ABBA mean or capacity claim. The mechanism was active: B1 used eight
   persistent threads for 29,128 tickets across 3,641 pool calls, with zero
   contended submits, while A1 created 34,936 threads across a different helper-call
   total. The exact evidence and hashes are in the
   [signature-pool result](benchmarks/results/native-signature-pool-20260913.json).
   Build per-account dependency components so
   independent source/destination chains execute concurrently and commit in stable
   original order. In the earlier balanced-control diagnostics, replay was
   45.69% of validation residence; fourfold replay acceleration gives a conditional
   **1.521× validation-latency** bound and ideal removal of 1.841×.

2. **Coordinate durable writes and cohort commits across validator stores.**
   Accepted state waits through CellDb and synchronous RocksDB writes; consensus
   namespaces also commit through synchronous transactions. More memory does not
   remove this durability wait. Instrument enqueue-to-actor delay, immutable prepare
   CPU, commit wall/thread CPU, `fsync`/`fdatasync`, WAL bytes, mutex/write stalls and
   records per sync. A throwaway-chain `sync=false` run is a ceiling diagnostic: if
   canonical TPS does not move, stop. If it does, add one ordered WAL coordinator,
   queue records for a bounded 250–1,000 µs or byte-count window, sync once, and
   resolve each promise only after its LSN is durable. A1's final 10-minute rolling
   `celldb_store_cell.load` was 0.317879. This scoped wall-load counter is neither
   CPU attribution nor an established serial fraction. Treating all of it as serial
   gives optimistic conditional bounds of **1.313×** at fourfold acceleration and
   1.466× at ideal removal.

3. **Publish complete immutable canonical epochs and application-time deltas.**
   Admission should pin one epoch containing the masterchain/config identity, all
   referenced shard roots and account observations. Publish it atomically only when
   complete. Generate account changes while applying canonical state, promote only
   shard-client-confirmed paths, and feed producers ordered append/remove/advance/
   invalidate operations. Gaps, forks and topology changes fall back to exact full
   reconstruction. This differs from the failed journal, which performed a post-hoc
   dictionary diff and still walked every tracked source. At A1's endpoint,
   cumulative shard-wait wall sum was 5,153.746208 s against 27,496.370788 s of
   aggregate batch residence (18.74%). Concurrent batch wall sums are neither
   strict measured-window attribution nor a serial fraction. Treating the ratio as
   serial gives a conditional **1.164× batch-latency** bound at fourfold acceleration
   and 1.231× at ideal removal.

4. **Schedule pure lane work on a global work-stealing arena.** Keep mutation,
   durability and root publication ordered per lane, but submit immutable account
   cell construction, private trie branches, hashes, proof subtrees and serialization
   to shared workers with lane tags, deadlines and weighted fairness. This tests
   whether fixed scheduler islands contributed to the observed aggregate CPU
   headroom; per-group utilization was not measured. Aggregate small trie jobs
   instead of creating raw threads per checkpoint. Native commit represented
   14.43% of measured collator residence;
   fourfold acceleration gives a conditional **1.121× collator-latency** bound and
   ideal removal of 1.169×.

5. **Construct authenticated state incrementally from changed paths.** Retain an
   immutable exact-parent root, dirty-path journal and reversible subtree/UsageTree
   accounting; update each changed ancestor once and seal one root/proof. Earlier
   overlay and direct-trie experiments did not improve end-to-end TPS, so keep this
   behind the higher-signal validation, durability and scheduling work. Require
   bit-identical roots/proofs for repeated accounts, shared paths, rollback, size
   boundaries and branch changes.

A 2×2 owners/domains experiment (0/0,4/0,0/4,4/4) can identify which switch caused
the combined loss. Lane-homogeneous admission batches can separately test whether
the slowest-owner join barrier matters. Both are diagnostics; neither addresses
the shared validation and durability paths that now have stronger evidence.

Implementation order: keep the persistent executor disabled; run the callback-cap
ABBA as a separate treatment; instrument durability and run the unsafe
ceiling test; then prototype ready epochs and the work-stealing arena. Keep each
treatment separate. Every live gate requires repeated 600-second strict-image comparisons,
complete canonical proofs, zero residual drain, stable identities and at least 5%
useful offered-load surplus before a capacity claim. A 200k claim must use full-window
proof-observed canonical throughput with a load source capable of at least ~210k
useful transfers/s.
