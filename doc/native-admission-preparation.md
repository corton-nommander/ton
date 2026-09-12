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
lifetime and preserve single-message duplicate/broadcast semantics. This change
has no benchmark result until that gate and controlled performance runs finish.
