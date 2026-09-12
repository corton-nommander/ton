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

Latest runtime commit `ace22335` adds three default-off experiments: bundled
masterchain prerequisites, immutable checkpoint proof bases with trial overlays,
and cached immutable candidate encoding. `d9ccbb93` records their initial tests.
The new client-source pin commits in MyLocalTonDocker (`de220c6`, `ff92e1d`,
`941ca59`) make comparisons reproducible; they do not optimize validator execution.

The resumed local series completed two 600-second runs: original image **52,525.04
canonical / 52,414.72 offered/admitted**, then new image with all experiments off
**55,776.96 / 55,765.49**. Both passed proof/drain and identity checks. The +6.19%
sequential whole-image observation has no repeat and cannot be attributed to a
feature. Its host-bound database, rebuilt common image wrapper, maintenance and
profiling conditions differ from the historical 61k series. The next context arm
was interrupted after genesis/client/stats containers disappeared; only 26.8s of
measurement was observed, with no final proof receipt. Later arms were withheld.
No new experiment is promoted. Raw identities, diagnostic joins and results are
saved in the [recovery record](benchmarks/results/native-collation-recovery-20260912.json).

The separate resumed diagnostic gives useful direction. An exact local
consensus-candidate/block join found 1,911 accepted basechain blocks in 120s:

| Region | Mean per locally accepted block |
|---|---:|
| Full collation wall interval | 164.940 ms |
| Waiting for external work | **109.003 ms** |
| Native commit, inclusive | 14.506 ms |
| Staged dictionary updates | 8.274 ms |
| Checkpoint reconstruction | 3.548 ms |
| State Merkle update | 10.266 ms |
| Initial block/proof BOC construction | 4.665 ms |

First useful-work waiting contributes 75.08% of the external wait. The separate
23.245s instrumented prerequisite sample averaged **5.671ms/request**: 2.227ms in
named actor queues, 3.436ms in asynchronous waits, and 0.00788ms in selection and
reference service. This does not demonstrate improvement in external-work wait.
A distinct sample confirmed **374 of 375 candidate roots encoded twice**, for
broadcast and storage. Encoding's logged mean 5.036ms excludes initial BOC decode
and comes from a different population. Parent/child regions overlap; these
figures cannot be summed into CPU attribution or a TPS forecast.

The following ranking is by architectural scaling potential. Numeric targets are
**prototype goals or conditional models, not predicted/measured TPS gains**, and
must not be multiplied together. Items 2–4 are useful foundations for item 1.

1. **Partition admission, execution and state publication together by lane.**
   Collation, block production and state construction already have per-shard
   actors. Isolate those existing pipelines from shared admission, manager and
   storage scheduling; assign each execution domain explicit resources and
   ownership of its account view, reservations and publication. A coordinator handles
   canonical commitments and topology transitions instead of per-transfer work.
   First prototype separate execution domains on one machine; then evaluate
   independent processes/servers and explicit routing. Replicas that execute the
   same full workload still serve replication/security, not aggregate scaling.

   **Goal: 2× aggregate throughput in an initial split, with 3–4× as a multi-group
   stretch target using additional independent compute.** Four-way Amdahl scaling
   needs about 92% of the actual limiting path parallelizable to reach 3.228×;
   eight-way needs about 79%. Those fractions have not been measured. The earlier
   four-pool-owner treatment already regressed to 46,119 TPS versus 55,913/53,300
   controls. The redesign must isolate existing downstream shard actors from
   shared scheduling and resource bottlenecks. Preserve conserved capacity permits,
   exact roots, explicit cross-lane semantics and validator/security requirements.

2. **Keep a persistent producer and ready-work index for each lane.**
   Today a new candidate installs a queue/callback, establishes an epoch and
   prefills work. Replace that lifecycle reset with a long-lived immutable ready
   index; each candidate takes a small lease/cursor tied to its exact parent
   branch. Prepare the next eligible prefix while its predecessor seals. Release,
   cancellation and consumption remain specific to that branch. Existing immediate
   dispatch and saved cursors already handle simpler versions of this idea.

   **Initial benchmark goal: 1.3–1.5× observed TPS if sustained eligible work is
   currently stranded during producer setup.** A toy per-block calculation removing
   half of the 109ms external wait gives 1.49× lower latency, not a TPS prediction.
   Some waiting is packing/backpressure and cannot simply be deleted. First prove
   ready work exists while the consumer waits, then measure install→first useful
   publication→wake and batch density. Keep broad checkpoint retention off and all
   speculative nonce floors callback-local.

3. **Prepare messages off the admission owner; select exact state immediately
   before a short reservation commit.**
   Decode and verify immutable, hash/domain-bound parents in independent workers.
   Bind them to the current exact account/configuration view only at the final
   owner-local reserve→insert→commit step. This reduces the interval during which
   an asynchronously pinned state can become stale. Revalidate all state-dependent
   authorization, nonce, expiry, balance, locality and mode conditions there.
   A valid signature alone never authorizes spending.

   **Initial benchmark goal: 1.2–1.5× if owner turns remain the measured limit.**
   An earlier experimental diagnostic found the pool near one saturated core and
   about 36.4ms from verifier completion to pool continuation. Reprofile the
   retained build before treating that as current attribution. This redesign moves
   work/state boundaries; previous verifier-task batching and bounded snapshot
   refresh did not demonstrate a gain. Gate on less owner service per admitted
   parent and fewer total validation passes, not only fewer retry responses.

4. **Drive reconciliation from a verified canonical account-change journal.**
   Publish contiguous predecessor/successor roots and changed account states only
   after canonical application. Update pending reservations and readiness from
   those changes instead of repeatedly looking up every tracked account. Include
   incoming destination balances, deletions, mode/topology changes and expiry from
   canonical block time. Gaps, unsupported reorganizations or missing provenance
   fall back to the existing exact-root scan.

   **Initial benchmark goal: 1.1–1.3×; larger claims need a new CPU profile.** An
   earlier diagnostic found 837,275 unchanged results in 1,174,802 account applies
   (~71%). This is redundant scan work, not 71% of total execution. Fine scan
   chunks made snapshots stale and regressed; a committed delta stream changes the
   source of truth. Prove journal/full-scan equivalence and bounded replay before
   measuring. Candidate acceptance stays a hint; only canonical application can
   advance the global watermark.

5. **Construct authenticated state incrementally from changed paths.**
   Use an immutable exact-parent root, a dirty-path journal and reversible shared
   subtree/UsageTree accounting. Update each changed ancestor once, checkpoint the
   journal, and seal the resulting root/proof once. Retain mandatory early size
   limits and exact rollback. This eliminates repeated traversal, not merely
   copies of proof-stat sets. The current overlay screen was 12.94–23.10% slower;
   direct trie-stage speedup also failed to improve end-to-end TPS.

   **Expect a modest isolated opportunity; use a 5–15% prototype target, not 2×.**
   Even a zero-cost toy limit for the separate dictionary/checkpoint/Merkle regions
   gives only ~1.155× lower collation latency in this diagnostic. Elimination of
   checkpoint reconstruction alone gives ~1.022×. Benefits may also reduce CPU and
   allocation pressure, which must be measured. Require bit-identical roots/proofs
   against the serial reference for overlapping paths, repeated accounts, shared
   subtrees, rejected trials, size boundaries and branch changes.

An additional foundation is a sealed candidate artifact containing decoded block
and state roots, exact-parent identities, metadata and encoded representations.
Producer, resolver and accepter currently reconstruct parts independently.
Reusing verified immutable artifacts can support lane isolation while preserving
independent validation and strict remote decoding. The existing payload cache
only covers encoding; its **66.46% three-consumer component saving is not 3× TPS**.
The observed ordinary path has two consumers. Bound retained bytes by candidate
lifetime/session and measure RSS as well as throughput.

Implementation order: profile the retained production build with stage/queue and
ready-work timestamps, prototype item 2, then item 3 and canonical deltas from
item 4. Use those ownership boundaries to attempt item 1. Item 5 is secondary
unless a new profile shows state construction limiting scaling. Keep each treatment
separate and retain a passing baseline after every prototype.

Each gate requires repeated 600s strict-image comparisons with identical
connections, 20ms coalescing and initial/max logical windows 32,768/65,536; complete
canonical proofs, zero residual drain, stable identities and useful offered-load
headroom. Inspect offered/admitted/canonical rates, window-cap pressure, density,
RTT, follower backlog, per-owner CPU and memory. The existing higher-window
experiments did not show persistent eligible work blocked at the cap. More
connections, lanes or shorter failure timeouts have no demonstrated path to 3×.
A 200k claim must use independently observed canonical logical throughput over
the full interval, with a load source capable of at least ~210k useful transfers/s.
