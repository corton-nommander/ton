# Native direct runs with retained canonical roots

`TON_NATIVE_DIRECT_RUN_ZERO_COPY=1` enables an experimental local representation
for v5/v6 signed NTRN batches. It is **off by default**. The collator and validator
retain strictly decoded parents and their canonical cells, avoiding the expanded
`NativeTransferBatchEntry` vector and its 64-byte signature copy for each logical
output. This implements a work reduction. The completed
[live ABBA test](native-rooted-runs-tps-2026-09-14.md) found no repeatable TPS gain:
56,875 TPS with the default representation versus 56,779 TPS with rooted runs.

The compact [result record](benchmarks/results/native-rooted-runs-20260914.json)
records validation and microbenchmark evidence. Raw artifacts
remain under `build/benchmarks/native-rooted-runs-20260914/`. Do not substitute
provisional microbenchmark output for that result record.

## Representation and canonical checks

`CanonicalNativeTransferRun::unpack_external(cell)` calls the existing strict run
parser. That parser validates fields, bounds, signature length, nonce range and
output-tree structure, rebuilds the canonical parent, and compares its hash with
the input. The wrapper privately owns the decoded run and this rebuilt root. It
exposes a const run view and a root reference; callers cannot construct a wrapper
by supplying unrelated fields and cells. A seal establishes canonical wire
identity, not cryptographic authorization.

`NativeTransferBatch::unpack_rooted_runs(cell)` shares the existing batch parser.
For v5/v6 it returns ordered `rooted_runs`, with `entries`, `runs` and `accounts`
empty. It checks each branch's logical weight, total protocol bounds and the
canonical outer tree just as the expanded decoder does. v1-v4 preserve the
existing expanded decoder semantics. The public `unpack()` and parent-metadata
projection retain their previous representations and validation rules.

Rooted serialization combines the sealed parent roots using the original ordered
pairing and logical-output weights. It rejects a nonempty rooted vector alongside
any expanded entries, runs or accounts, and rejects moved-from seals with no root.
`logical_entry_count()` counts outputs for direct runs and entries for scalar
batches; serialization and decoding still enforce the protocol bound. The wire
version, root hash and serialized BOC remain identical for equivalent input.

## Collation and validation

The collator retains each canonical parent through pending checkpoints and
commits its root only with the accepted run. Logical-output counts drive capacity,
checkpoint and nonempty-batch decisions even though the expanded entry vector is
empty. Serialization reuses the committed canonical roots.

The validator independently parses and checks the candidate's batch. It collects
the same source/destination set from the parents, verifies the complete run
signatures under the existing chain-domain rules, and replays every output in
wire order. The v6 fixed payment-lane and current-shard locality checks remain in
the existing consumers.

Both consumers use `CanonicalNativeTransferRun::for_each_transfer(callback)`.
It initializes one reusable `NativeTransfer` scratch per parent, copying that
parent's signature once, then updates destination, amount, fee and nonce for each
output. The callback borrows a const reference valid only during that call and
stops traversal on its first false result. Child signatures are not independent
authorizations: skipping their verification is valid only after authenticating
the parent. Replay uses the existing state engine and preserves source checks,
expiry, nonce, balance, fee and failure order. This is not parallel replay and
does not reuse another validator's replay result.

## Control and evidence

```dotenv
TON_NATIVE_DIRECT_RUN_ZERO_COPY=0
TON_NATIVE_VALIDATION_SIGNATURE_CACHE_FASTPATH=0
TON_NATIVE_VALIDATION_SIGNATURE_PERSISTENT_POOL=0
```

Only the literal `1` enables the rooted path; unset, `0`, empty and other values
leave it disabled. The process caches the setting on its first read, so recreate
the validator to change it. Existing protocol-version and capability gates still
decide whether direct signed runs are allowed. Enable this experiment separately
from the signature-cache preflight, persistent executor, validated-state handoff
and scheduler experiments.

Collation and validation report `native_rooted_run_parents`,
`native_rooted_run_outputs` and `native_expanded_entries`. A rooted nonempty v5/v6
batch should report positive parent/output counts and zero expanded entries.
Validation additionally reports `native_replayed_outputs`; successful replay must
cover the complete logical batch. These are per-operation work diagnostics,
including attempted candidates, not canonical throughput accounting.

The new validation `native_batch_decode` wall/CPU timer covers the strict batch
decoder on an already available cell root. It excludes BOC deserialization,
signature verification, account collection and state replay. The existing
`native_batch_serialize`, `native_signature_verify` and `native_state_replay`
timers supply separate observations. Do not add overlapping timers as exclusive
CPU attribution.

The validator and generator builds pass with two low-priority compiler jobs.
The metadata suite passes 13 cases, `test-cells` passes 40 cases,
`test-native-work-scratch` passes five cases, and the collation telemetry suite
passes 15 cases. The companion reporting suite also passes. Coverage
includes differential root/BOC equality, scalar fallback, empty and maximum
batches, parent ordering and duplicates, malformed and noncanonical trees,
ownership and mixed-representation rejection. The shared callback is compared
with expanded replay through the actual state engine after parent authentication,
including self-transfers, repeated destinations, a mid-run insufficient balance,
nonce mismatch, expiry/error precedence and terminal-nonce overflow.

The microbenchmark compares strict full/rooted decoding, expanded/rooted store,
flattening and scratch traversal, plus paths including BOC decoding. Its stages
overlap and measure warm-cache wall time without signature verification. They
describe local codec work, not validator TPS or network capacity.

The final seven-repetition run finished after task compilation stopped. For v6
batches of 65,536 outputs, median component times were:

| Workload | Expanded decode | Rooted decode | Expanded store | Rooted store |
| --- | ---: | ---: | ---: | ---: |
| Repeated destination/output | 74.41 ms | 40.77 ms | 34.60 ms | 1.18 ms |
| Distinct outputs | 83.81 ms | 43.78 ms | 38.74 ms | 1.36 ms |

This is 1.83–1.91× faster decoding and 28.51–29.37× faster serialization in this
microbenchmark. Earlier measurements taken during compilation remain separate
exploratory artifacts. The result record includes all selected v5/v6 comparisons,
raw output hashes, build/test logs and the executable hash.

The current companion reporter processed the archived 163,008,240-byte A1
session log in 25.33 seconds. All previous summary fields matched the archived
result, and all eight new representation populations correctly marked the old
image's missing telemetry unavailable. This extends the earlier byte-identical
indexed-parser check with the new additive reporting fields.

The live calibration, rooted smoke and four ten-minute ABBA arms completed with
canonical proofs, zero residual drain and fixed workload/runtime identities.
Both rooted arms exercised actual Collator/ValidateQuery handling and checkpoint
commits with complete representation/replay counters. No checkpoint rollback
occurred, so that runtime branch remains unobserved. Rooted serialization and
decoding used less candidate wall time, but the TPS mean changed by only −0.17%
while the controls drifted by −6.18%. Keep the flag off. No arm established the
independent useful offered-load headroom needed for a capacity claim. See the
[live result record](benchmarks/results/native-rooted-runs-tps-20260914.json) for
the complete comparison and its separate correctness/capacity decisions.
