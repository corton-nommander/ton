# Local overlay signature reuse experiment

`TON_OVERLAY_LOCAL_SIGNATURE_REUSE=1` enables a default-off optimization for
simple and FEC broadcasts signed by the local keyring. The flag is captured
when each overlay actor is constructed. Unset, `0`, and other values keep
ordinary verification, without allocating signing receipts.

The separate desktop CPU capture on 2026-09-09 found 14.274% of sampled user
CPU in overlay signature checks reached through successful local signing
callbacks: 8.763% FEC and 5.511% simple. This is a CPU attribution, not a
measured TPS improvement. Paired throughput measurements use the same prebuilt
image and vary this flag independently of `TON_KEYRING_PREPARED_SIGNING`.

An outgoing request optionally owns an in-memory receipt containing an
independent copy of the exact serialized `overlay.broadcast.toSign` bytes and
the requested signer identity. Only its first successful keyring completion
with the requested Ed25519 public key and a 64-byte signature completes the
receipt. The signature is copied independently, not cloned into shared mutable
storage. The reuse check matches the exact public key, signed bytes, and
signature against this receipt. Missing, incomplete, unsupported, or mismatched
evidence falls back to ordinary cryptographic verification.

This trusts the existing local keyring contract that a successful signing
reply signs the bytes supplied with that request. It does not introduce a
general positive-signature cache. Receipt construction is private to the local
simple/FEC send paths; decoded network messages never receive one. A zero peer
ID, a claimed local source, `AnySender`, and the existing `is_ours` bookkeeping
cannot authorize reuse. Separate overlay actors do not share receipts.

The existing temporary peer ban is checked before reuse. Certificate and
source eligibility, dates, broadcast/part hashes, FEC parameters and decode
checks, duplicate detection, rate/accounting limits, application approval,
delivery, and propagation retain their existing paths. Incoming simple, full
FEC, and short FEC messages always use ordinary signature verification.

The existing overlay stats list includes `local_signature_reuse`, containing
`enabled`, `receipt_checks`, `hits`, `mismatches`, and `crypto_checks`. Counters
belong to the overlay actor and need no atomics; rejected banned peers do not
increment reuse or crypto checks. Certificate verification is included in
`crypto_checks` and never receives a local broadcast receipt.

`test-overlay-local-signature` checks immutable receipt bytes, typed callback
failures, wrong keys and unsupported signing kinds, control mode, ban ordering,
real keyring callbacks and delivery for both broadcast forms, and remote
simple/full/short FEC validation despite zero peer IDs and local identities.

## Desktop measurement and default scope

The September 9 A/B/B/A test used the same `be235e03` images, four lanes,
10 connections, 600 measured seconds and 20 ms coalescing per arm. Controls
averaged 57,673.19 canonical logical TPS; reuse averaged 60,039.59 (**+4.10%**).
Both candidates exceeded both controls, with **9.39% lower sampled validator
CPU** on average. Proof/catch-up, complete drain and image checks passed.
See [the report](native-admission-cycles-2026-09-09.md) for all arms, the
duration reassessment and retained failures.

Only the tested MyLocalTonDocker `.env.desktop` now enables reuse. The C++ and
Compose fallback, and `.env.physical`, remain off. Prepared-key signing, admission
sharing and snapshot refresh remain off in the desktop winner. These local
images are not published production images. The test establishes a repeatable
observed improvement; offered load did not establish maximum capacity.
