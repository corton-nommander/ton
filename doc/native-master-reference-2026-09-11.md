# Master performance reference and next steps — 11 September 2026

The requested roughly 61k offered/admitted version is **metadata projection at
`7b73cdb157a1f5e621e420e1e5c913726b267cf5`**, with local overlay signature reuse.
It is already an ancestor of local and remote `master` at `8c97aa23`. Runtime
source is unchanged between those revisions; intervening changes add evidence,
documentation and the image-publication correctness gate. No merge of the later
experimental branch is needed to obtain the winning implementation.

| Measurement | Offered/admitted logical TPS | Canonical native TPS |
|---|---:|---:|
| Projection enabled | 61,654.45 | 61,380.86 |
| Projection enabled, repeat | 62,524.27 | 62,537.54 |
| Candidate mean | 62,089.36 | 61,959.20 |

Matched controls averaged 60,201.79 canonical TPS: the observed gain was **2.92%**,
with both candidates above both controls. All four arms completed 600 seconds,
proof following and drain. Maintenance before the final control limits causal
interpretation; useful offered load lacked independent 5% overdrive. These are
observed desktop rates, not a capacity guarantee. The [original report](native-candidate-metadata-cycles-2026-09-09.md)
and [reference record](benchmarks/results/native-master-reference-20260911.json)
retain exact values, ancestry and evidence hashes.

## Reference and later experiments

The historical reference enabled metadata projection, local overlay signature
reuse and admission configuration caching. Prepared signing, admission sharing,
snapshot refresh and broad checkpoint retention were off. Its reconciliation
timing flag was on; changing profiling is a separate treatment.

It used four lanes, 24,576 sources, ten connections, six client workers/signers,
sixteen transfers per parent, batch/query limits of 64, 20 ms coalescing, and
initial/maximum logical windows of 32,768/65,536. Validator/client/stats CPU
quotas were 16/6/1 inside a 24-vCPU, 64-GiB Docker Desktop VM, with eight native
executor/admission workers and eight block signature helpers. The database used
a Docker named volume. The JSON record includes the comparison settings and
historical companion preset revision. Preserve existing production topology and
data; this is not an instruction to convert an eight-lane chain to four lanes.

The companion `.env.desktop` was later set to another local reference: one owner
and a 16,384 window on runtime `5e413ff0`. Checking out TON `master` does not change
that file, Docker images or running services. Current `.env.physical` already
enables metadata projection, overlay reuse and configuration caching with
retention off; its hardware, topology and client limits differ from this desktop
measurement.

Experimental runtime changes remain on `perf/native-dispatch-freshness-20260910`
at `80816c32`. Its equal-window 600-second control/four-owner/control sequence
observed **55,913 / 46,119 / 53,300 canonical TPS**, with more CPU use and backlog
in the candidate. Storage and host conditions differed from the 61k cycle. This
rejects promotion of that owner treatment; it does not prove that checking out
earlier code alone restores an absolute rate. The reference record identifies
the experimental evidence blobs without importing its runtime changes.

## Prioritized implementation plan

First profile the retained reference on the intended server with fixed images,
state, resources and useful load. Separate request queue, service and callback
scheduling time. Existing wall timers overlap, and `work_time` omits separately
timed native work; subtraction cannot identify an unexplained idle phase. The
losing experiment supplies leads, not a CPU ranking for the winner.

1. **Provide one exact collation prerequisite context.**
   `Collator::request_top_masterchain_state` obtains a minimum state, then asks
   for the latest state; `after_get_mc_state` separately requests the selected
   masterchain block for its reference. Return the selected immutable state,
   complete block ID and once-validated external-block reference together where
   available. Preserve minimum/parent requirements, hardfork behavior, original
   deadlines and exact fork identity. Block-data and neighbor-proof requests
   already coalesce; another generic cache is not this change. Measure each
   request boundary first, then remove demonstrated extra actor round trips.

2. **Reduce checkpoint proof-stat reconstruction.**
   `flush_pending_checkpoint` copies original proof-stat sets and traverses the
   evolving accounts root repeatedly. Begin with an immutable base plus an owned
   trial overlay in `NewCellStorageStat`; benchmark actual account counts and
   shared subtrees. Only then consider reuse of replaced proof paths. Superseded
   staged roots must not accumulate in accounting. Differential tests must
   preserve UsageTree boundaries, rollback, hard limits and actual serialized
   block sizes. Broad checkpoint retention stays off.

3. **Serialize a sealed candidate once.**
   `create_block_candidate` creates block/collated BOCs; `Candidate::serialize`
   can run again for broadcast, persistence and later requests. Compression
   deserializes and reserializes those BOCs. Publish an immutable payload and
   reuse complete serialized bytes with a bounded lifetime. Candidate fields are
   public and mutable: an unchecked cache keyed only by root hash is unsafe.
   Preserve every envelope/compression dependency, byte-identical output,
   hashes, size checks and strict remote decoding.

Later candidate timings motivate these targets: masterchain block-data callbacks
rose about 3→132 ms per candidate; checkpoint reconstruction rose 1.06→2.37
microseconds per candidate transfer; serialization rose 1.38→2.31. Different,
overlapping populations cannot be added or converted into promised TPS gains.
Original evidence is available through
`git show 80816c32:doc/benchmarks/results/native-owner-client-20260910-stage-evidence.json`.

Implement one change at a time. Prebuild images, then repeat 600-second A/B/B/A
comparisons with identical client settings. Compare measured offered/admitted
and canonical TPS, RTT, batch density, cap pressure, backlog and CPU. Preserve
exact state and branch-local nonce floors. Keep a candidate only after a
repeatable end-to-end gain with complete proof and drain. Raise credits only
when ready work repeatedly waits at the cap without growing downstream backlog.
Larger blocks and shorter failure timeouts are not assumed improvements.

Reaching 200k from 61,959 requires roughly **3.2 times** the useful throughput.
There is no measured basis to promise that from parameters alone. If shared
state processing/finalization remains limiting, evaluate distributing that work
by lane/shard. Extra admission owners or validators repeating the same work do
not demonstrate capacity growth. Hardware changes should follow measured CPU,
memory or storage saturation on the intended servers.
