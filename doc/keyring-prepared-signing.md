# Prepared Ed25519 signing keys

`TON_KEYRING_PREPARED_SIGNING=1` enables a default-off experiment in the validator's keyring. The keyring reads the flag when it is created; only the literal `1` enables it. Restart the validator to change the setting, and use the same prebuilt image for both measurement arms.

Each loaded Ed25519 key's signing actor imports its immutable private key into OpenSSL once, on the first successful signing request. Later scalar and batch requests reuse that prepared key, while every request still signs its own exact input using a fresh signing context. This changes neither signed bytes nor the signature format. Failed imports remain retryable. Key deletion releases the prepared key with the existing signing actor; there is no global cache, shared signing context or additional owner retaining deleted keys.

The original private/public key import, export and decryption paths remain in use. Other key types and native load-generator direct signing are unaffected. The wrapper is used only when constructing a keyring signing actor.

The desktop CPU diagnostic on September 9 attributed 4.533% of sampled validator user CPU to private-key import/public-key derivation beneath signing, and another 4.408% to the signing operation itself. Prepared-key reuse targets the former. These sampled CPU shares are neither measured TPS gains nor a guarantee that all import overhead disappears. Compare canonical proof-checked TPS, complete drain, matching workloads and image identities before changing the default.

`test-keyring-prepared-signing` covers deterministic scalar/batch signatures, empty and binary inputs, distinct domains and keys, preparation reuse and destruction, malformed-key errors, delegated decryption, startup flag behavior, and real keyring actor export/reload/delete/re-add behavior in both modes.
