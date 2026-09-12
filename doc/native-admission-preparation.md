# Native admission preparation experiment

`TON_NATIVE_ADMISSION_PREPARE=1` moves strict external BOC decoding, native
materialization and domain-bound signature verification onto the existing
signature worker actors. The default is `0`; worker count does not change.
Single-message and batch RPCs share the preparation boundary, while retaining
their existing distinct result and broadcast-waiter semantics.

Preparation produces an owned, sealed decoded message. It carries its original
wire size, root/source identity, parsed scalar or intact signed-run fields and
the signature result for one zero-state signing domain. No account state,
reservation, nonce watermark or callback state is accessible to the workers.
The existing strict canonical native parser and positive signature cache are
reused; this does not introduce another cache or weaker parser.

The initial configuration read supplies only a decoding bound and signing
domain. The owner selects its spending snapshot after preparation returns.
Every physical representation, including duplicate BOCs with the same root,
must meet the newly selected size/depth limits. Current mode, lane locality,
account kind, nonce, balance, expiry, exact snapshot/source revision,
reservation, insertion, commit and rollback checks remain authoritative.
Prepared authorization cannot cross a signing-domain change. Signature failures
are retained until the normal source precheck stage to preserve error
precedence. Generic execution and single-message idempotence are unchanged.

At most 4,096 physical inputs and 16 MiB of input bytes can be queued through
preparation. A request holds a shared lease until its last worker completes,
even if the caller or absolute RPC deadline has already expired. Capacity is
returned only on the owner actor. Saturation and unavailable preparation config
use the original admission path with the original input bytes; they do not
create a new RPC error or extend its deadline. Early verification may do work
for inputs later rejected on mutable source state; measure this overhead.

`TON_NATIVE_ADMISSION_PREPARATION_PROFILE=1` adds optional timing observations;
its default is `0`. The existing batch diagnostics expose `prepare_*` counters
for requests, physical inputs, current/peak charged parents and bytes, hard
limits, fallback causes, consumed authorizations and domain mismatch rejection.
The timing families are:

- `prepare_queue`: request dispatch timestamp to each worker entry.
- `prepare_decode`: strict worker decode/materialization wall time.
- `prepare_signature`: worker verification wall time, including cache lookup.
- `prepare_worker_cpu`: worker thread CPU, excluding async waiting.
- `prepare_item_to_batch_resume`: each completed worker to owner batch resume;
  this includes waiting for slower siblings, not solely actor mailbox delay.

With preparation enabled, existing `decode` and `verification` batch timers
cover their remaining owner work; worker work is reported separately. These
timers overlap or use different populations and must not be summed as exclusive
latency/CPU. Use detailed profiling only for attribution, then disable it for
fixed-image A/B comparisons of offered/admitted/canonical TPS, snapshot retries,
RTT, charged preparation work, batch density and final proof/drain.

`NativeAdmissionPreparation` actor fixtures live in the existing
`test-native-admission-refresh-batch` target, already included in the native
publication gate. They hold real preparation replies across state/config
changes, run strict signed BOCs and account dictionaries, verify deadline lease
lifetime and preserve single-message duplicate/broadcast semantics.

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

Performance result: stage3 was **inconclusive**, so preparation remains **off**
in the selected reference profile. Only three full 600-second arms completed;
the second control exceeded the first treatment, and the last treatment stopped
after a telemetry timeout. That interrupted arm is not a valid performance result.
No repeatable preparation gain or capacity increase was established. See the
[stage3 decision](../build/benchmarks/native-lane-pipeline-stage3-fresh-20260912/winner-decisions.json)
and [12 September cycle report](native-lane-pipeline-cycles-2026-09-12.md).

The implemented [four-owner prototype](native-prepared-lane-owners-2026-09-12.md)
requires preparation enabled. Its comparison therefore enables preparation in
both control and treatment; that dependency does not independently promote this
feature or compare the combined profile with the preparation-off reference.
