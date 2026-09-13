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

Before another source redesign, use the callback-stage telemetry to split the
existing 56.661/62.225 ms mean pre-epoch wait into Collator-to-Manager,
Manager-to-pool and pool-entry-to-first-epoch intervals. The two saved arms spent
213.896304/184.745156 seconds before the first producer epoch across 3,775/2,969
uninstalled waits. Their final ExtMessagePool actor-type load was 0.930228/0.903469,
with maximum turns of 1.572621/1.359085 seconds, but five actors shared that type;
the stage split is needed before attributing the delay to the main pool actor.

If Manager-to-pool or pool-to-epoch dominates, compare the generator's existing
64-query per-client cap with 12 in a same-process 64/12/12/64 sequence. Ten clients
give 120 credits. At the observed roughly 225/202 ms admission residence this is
about 533/595 queries/s of capacity, still 38%/64% above the measured 386.2/363.3
queries/s, while the observed per-client maxima of 23/22 ensure that 12 exercises
the bound. Stop if stage latency does not fall, offered load falls, or either
treatment loses canonical TPS. This experiment changes only the generator and can
reuse one validator process and image.

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
   remove this durability wait. The default-off CellDb probe now reports queue wait,
   immutable prepare CPU, write-batch construction, blocking `DB::Write`, RocksDB
   WAL sync/bytes/stalls and end-to-end scheduling for successful regular stores.
   A throwaway-chain `sync=false` run is a ceiling diagnostic: if
   canonical TPS does not move, stop. If it does, add one ordered WAL coordinator,
   queue records for a bounded 250–1,000 µs or byte-count window, sync once, and
   resolve each promise only after its LSN is durable. A1/B1 final 10-minute
   `celldb_store_cell` load was 0.306049/0.385831 at 7.800954/6.623238 calls/s,
   implying 39.23/58.25 ms of scoped wall time per call. The CellDb actor-type load
   was 0.439251/0.524598 and its maximum turns were 1.721064/1.305125 seconds. These
   scoped wall counters are neither CPU attribution nor established serial
   fractions. Treating A1's 0.306049 as serial gives optimistic conditional bounds
   of **1.298×** at fourfold acceleration and 1.441× at ideal removal.

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

6. **Measure duplicate local candidate BOC decoding before attempting a handoff.**
   Every A1/B1 validation matched a candidate collated by the same validator.
   Native candidate unpack averaged 20.628/25.158 ms wall and 19.678/24.048 ms CPU,
   but its aggregate CPU was only about 0.148 core per run and the timer includes
   work beyond BOC deserialization. Add an exclusive timer around the validator's
   second deserialize first. If material, pass only an immutable decoded root with
   full block ID, file-hash and root-hash checks through the transient validation
   request. Never retain a decoded batch or reuse replay results; missing or
   mismatched artifacts must take the existing decode path.

A 2×2 owners/domains experiment (0/0,4/0,0/4,4/4) can identify which switch caused
the combined loss. Lane-homogeneous admission batches can separately test whether
the slowest-owner join barrier matters. Both are diagnostics; neither addresses
the shared validation and durability paths that now have stronger evidence.

Implementation order: keep the persistent executor disabled; collect the callback
stage split and run the 64-versus-12 query-credit ABBA only if the pool boundary
dominates; run the three-arm CellDb profile-off/profile-on/sync-false ceiling;
then run the 18,432-versus-8,192 callback-cap ABBA. Prototype ready epochs and the
work-stealing arena only after those screens. Keep each treatment separate. Every
live gate requires repeated 600-second strict-image comparisons,
complete canonical proofs, zero residual drain, stable identities and at least 5%
useful offered-load surplus before a capacity claim. A 200k claim must use full-window
proof-observed canonical throughput with a load source capable of at least ~210k
useful transfers/s.
