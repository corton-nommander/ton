# Collation experiments — 11 September 2026

Implementation: `ace22335`, branch `perf/native-collation-payload-20260911`,
based on the retained master reference. Companion commit `203165c` exposes the
switches in Compose. All experiments remain **default-off**; nothing was merged
to master or published. Neither `.env.desktop` nor `.env.physical` was changed.

| Experiment | Switch | Implementation and status |
|---|---|---|
| Masterchain prerequisites | `TON_COLLATION_MASTERCHAIN_CONTEXT` | Manager returns selected state, exact block identity, block and validated external reference together. Preserves minimum/parent selection, hardfork fallback and absolute deadlines. Needs live-chain measurement. |
| Prerequisite timing | `TON_COLLATION_PREREQUISITE_PROFILE` | With context enabled, separately reports manager queue, state callback queue, asynchronous state/block waits, selection/reference service and collator callback queue. Waits include downstream scheduling and I/O; they are not CPU measurements. |
| Checkpoint proof base | `TON_NATIVE_CHECKPOINT_PROOF_OVERLAY` | Owned immutable base plus fresh trial sets; accepted trials retain their base, rejected trials leave accounting untouched. Superseded account roots never accumulate. Microbenchmark regressed; leave disabled. |
| Candidate payload | `TON_IMMUTABLE_CANDIDATE_PAYLOAD_REUSE` | Candidate fields become read-only and input buffers are detached. Thread-safe once-only encoding retains the complete envelope; consumers receive independent output bytes. Promising component result, TPS not measured. |

`TON_NATIVE_CHECKPOINT_RETAIN_INGRESS=0` throughout. No global nonce watermark,
branch-floor, protocol, remote validation or block-size policy changes.

## Measurements completed

Five process repeats, Release/native build, compilation stopped before timing.
Unrelated desktop services remained active. These are **synthetic component wall
times, not TPS gains**. Payload run order alternated; checkpoint copy preceded
overlay within each process, so those measurements are screening evidence.

| Workload | Baseline median | Candidate median | Time change |
|---|---:|---:|---:|
| 100 candidates, 4,096 logical transfers each, three serializations per candidate | 90.387 ms | 30.312 ms | **−66.46%** |
| 100 proof checkpoints, 8,192 accounts, empty base | 231.415 ms | 284.862 ms | +23.10% |
| Same, 64 base accounts | 130.181 ms | 150.504 ms | +15.61% |
| Same, 4,096 base accounts | 133.558 ms | 150.846 ms | +12.94% |

All payload arms produced identical total encoded bytes. The proof overlay
eliminates base-set copies but retains full traversal of the evolving account
root and adds base membership probes. It was slower in every tested case;
avoiding traversal of unchanged proof paths is the next checkpoint problem,
requiring independent DAG/UsageTree accounting and rollback tests.

Input-buffer detachment is common to both payload masks. A comparison with the
original retained image is therefore also required before promoting this code.
One candidate's encoded bytes live only as long as that candidate; no global
payload cache was introduced. Strict remote decoding is unchanged.

## Validation and pending TPS runs

The production validator and generator build successfully. The mandatory
publication gate passed **398 TD test executions plus standalone resolver
assertions across 16 invocations**. Coverage includes shared proof subtrees,
UsageTree boundaries, replacement/rollback, base lifetime, tentative accounting,
exact reference identity and malformed input, and byte-identical empty, scalar,
native-run and multi-root payloads across concurrent consumers. Mutating input
aliases or returned encoded buffers does not change later serialization.

No validator or load was started for this cycle: root-disk free space was about
**4.7 GiB**, below the existing **8 GiB start / 5 GiB stop** guard. Build artifacts
were relocated and verified on the backup disk; a Docker trim returned only
134 MiB. The existing chain, wallets, unrelated services and disk guards were
preserved. **There is no new canonical TPS result or promotion.**

Checked local images are prebuilt as `mylocalton-{ton,genesis,client}:collation-local-ace22335`.
They are desktop CPU-specific, not registry images. Exact IDs, binary hashes,
gate evidence, samples and limitations are saved in the
[result record](benchmarks/results/native-collation-experiments-20260911.json).

Once root space is available: run a separate 120-second prerequisite timing
diagnostic; then compare fixed-image 600-second controls and isolated context /
payload arms, with 60-second warmup, 180-second drain, ten connections, six
workers/signers, 20 ms coalescing and a fixed 32,768/65,536 logical window.
Keep the existing four-lane database. Include the original image control, and
repeat promising candidates with profiling off. Require complete canonical
proofs and drain before account reuse. Compare measured offered/admitted and
canonical rates, RTT, density, cap pressure and backlog; require independent
offered-load headroom before claiming capacity.

Local resume artifacts: `build/benchmarks/native-collation-cycles-20260911/ACTIVE.md`,
`plan.json`, `env.*`, `run-arm.py`, and `runtime-ace22335/provenance.json`.
