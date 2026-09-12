# Canonical endpoint journal prototype — 12 September 2026

`TON_NATIVE_CANONICAL_JOURNAL=1` enables an experimental account-difference path
inside the existing canonical reconciliation walk. The default is **off**.
The flag belongs to the validator process environment; setting it only on the
load generator has no effect. The completed stage4 comparison demonstrated no
repeatable TPS gain, so the selected reference keeps it **off**. Broad ingress
checkpoint retention remains unrelated and must stay off.

The publisher is unchanged: `ValidatorManagerImpl::update_shard_client_block_handle`
sends the shard-client-confirmed masterchain state after all its referenced shard
tops have been applied. Local candidate acceptance and `ApplyBlock` are not new
watermark authorities. The journal verifies the full referenced shard block ID,
shard-state header and root before using its account dictionary.

For each shard, a completely successful reconciliation retains one immutable
accounts root and a sorted vector of decoded observations for the sources actually
checked at that endpoint. The next endpoint can skip intermediate shard heights:
`scan_diff` compares the two exact canonical dictionaries and prunes unchanged
subtrees by hash. Changed source/destination entries come directly from that diff.
An unchanged source reuses an observation only if that source was checked at the
retained predecessor. Newly pending sources still perform an exact dictionary
lookup. Incoming balance changes and deletions are included; invalid/deleted
native sources retain the original scanner's failure behavior.

Unchanged account facts do not imply unchanged reservation behavior. The fast
path still checks pending atomic intervals, canonical-time expiry, affordability
and account revisions. Any case requiring pruning/rebasing uses the existing
`apply_reconciled_native_account_state`. The no-effect path advances the observed
logical-time fence and source time, so a suspended admission observation cannot
regress them. Expiry uses the new **shard-state** time even when the accounts root
is unchanged. Speculative branch nonce floors never enter these observations.

A failed shard never publishes its new endpoint. Independent sources may already
have progressed, matching the existing full scan; replay is idempotent. Missing
successful predecessor markers, conflicting identities, backwards endpoint time
or unavailable/invalid old dictionary data fall back to the exact lookup path.
The existing public reconciliation entry still rejects backwards or competing
same-height masterchain updates. This prototype does not add canonical rollback
support or rewind the global nonce watermark.

Topology changes remove predecessor endpoints for shards no longer present.
Split/merge endpoints bootstrap with the existing lookup behavior. Retention is
bounded to 64 shard endpoints and 65,536 decoded source observations in total.
A diff stops after 65,536 changed accounts and falls back to lookup. The endpoint
roots retain immutable cell references; these bounds are not a byte-level RSS
limit, so RSS remains a benchmark gate.

This first stage still registers and groups pending sources and fetches each new
exact shard state. Diff construction runs synchronously on the pool actor and
uses a cheap per-source reservation walk rather than a separate expiry-index
lifecycle. It is intended to test whether removing dictionary lookups, decoding
and no-effect applications outweighs the new diff cost. It does not establish a
fully independent lane publisher or eliminate every reconciliation actor turn.

`native_reconciliation_telemetry_string()` exposes `journal_*` counters for
attempts/completions, bootstrap and fallback reasons, changed accounts, reused
observations, dictionary lookup bootstraps, no-effect application skips,
reservation checks, failed shards and retained source/endpoint counts. With
`TON_NATIVE_RECONCILIATION_PROFILE=1`, `journal_diff_*` reports diff wall time.
Existing `account_lookups` and `apply_calls` retain their actual-work meanings;
reused entries are not counted as dictionary lookups, and fast skips are not
counted as full applications. Compare measured-window deltas, not cumulative
since-start totals.

The focused `test-native-canonical-journal` target uses real native account
encodings/dictionaries and compares pool state and changed-source notifications
against an independent implementation of the original full scanner. It covers
new-source bootstrap, coalesced canonical endpoints, destination balances,
partial atomic nonce consumption, unchanged-root expiry, stale verifier revisions,
logical-time fencing, deletion/partial failure, malformed old diff fallback,
missing predecessor publication, topology eviction, exact-reference rejection and
32 mixed canonical transitions over 256 accounts.

The frozen `52a20df0` implementation passed **144 focused cases**, the native
publication gate's **21 invocations**, and all **seven frozen-runtime target
checks**. These are recorded executions, not newly rerun tests. The exact binary,
test and log hashes are in
[integration verification](../build/benchmarks/native-lane-pipeline-20260912/integration-verification-52a20df0.json)
and the [frozen runtime gates](../build/benchmarks/native-lane-pipeline-20260912/frozen-runtime-gates-52a20df0.json).
The later split-startup build `095bc0de` repeated the 144 focused cases,
21 publication-gate invocations and seven frozen-runtime checks. Its existing
mock-manager ordinary-idle regression also passed; a real fresh zero-ingress
split transition remains a separate pending gate. See the
[cycle report](native-lane-pipeline-cycles-2026-09-12.md#validation-and-next-context).

Performance result: all four 600-second stage4 ABBA arms completed with valid
proofs and drain. The journal removed substantial repeated dictionary work,
but treatment mean canonical TPS was lower and treatment/control ranges overlapped.
Chronological drift and changes in unrelated workloads prevent an isolated causal
regression claim. There is **no demonstrated TPS gain**, and the journal remains
**off** in the selected reference. See the
[stage4 decision](../build/benchmarks/native-lane-pipeline-stage4-volume-20260912/winner-decisions.json)
and [12 September cycle report](native-lane-pipeline-cycles-2026-09-12.md).

Any later promotion requires a new passing correctness gate for changed code and
repeated 600-second strict-image A/B runs with all other runtime experiments held
fixed. Keep 20ms coalescing and initial/max logical windows
32,768/65,536. Require complete canonical proofs, zero residual drain, stable image
and validator identities, useful offered-load headroom and unchanged correctness.
Compare owner CPU/turn time, journal diff time and fallback rate, dictionary
lookups/unpacks/applies, RSS, offered/admitted/canonical TPS, batch density, RTT and
follower backlog. Keep the feature off if the measured overall result regresses.
