# Native payment APIs in tonlib

The existing ADNL transport and `liteServer.sendMessage` RPC carry native BOCs. Native accounts use their Ed25519 public key as the basechain address. They have a balance, nonce and flags, and do not deploy a TVM wallet contract. An address must be funded through the sidechain's native funding/genesis process before it can pay.

Ordinary TON message construction, normalization and account APIs retain their existing behavior. These additive methods support native payments explicitly. Build applications against the updated tonlib schema to use them.

## Account address and state

`native.getAccountAddress(public_key)` is a static method: it accepts the existing tonlib public-key string format and returns a basechain `accountAddress`. It does not require library initialization, a network connection or a wallet deployment.

`native.getAccountState(account_address)` uses the existing proof-validated account lookup and returns:

- `address`, `block_id`, `sync_utime`: the account and observed chain context.
- `balance`, `nonce`: unsigned decimal **strings**, covering `0` through `18446744073709551615` without a signed-int64 or JSON-number conversion. Balance is in base units.
- `flags`: an integer from 0 through 255.

An absent or ordinary account returns an error from this native-specific method. Existing account reads now decode compact native accounts as well. The ordinary `AccountState` union gains a `native.accountState` constructor containing the same string balance/nonce and integer flags. Legacy `raw.FullAccountState` and `FullAccountState` balance fields remain signed int64: native values above `9223372036854775807` produce an explicit error directing callers to `native.getAccountState`, rather than wrapping or truncating. Standard contract account results are unchanged.

Native accounts are not treated as undeployed standard wallets during wallet-type guessing. Their nonce is the native account nonce; it is not a TVM wallet `seqno` or a transaction logical time.

## Constructing signed native messages

Both methods below use an existing tonlib `inputKeyRegular` and return a `native.message`. They sign and serialize only: they do not reserve a nonce, read a balance, submit a message or deploy a contract. `inputKeyFake` is rejected.

`native.createTransfer` takes:

- `input_key`, `destination`.
- `amount`, `fee`, `nonce`: unsigned decimal uint64 strings.
- `valid_until`: a future Unix timestamp fitting uint32.
- `chain_domain`: the 32-byte zerostate root hash, encoded as base64 in JSON. It must match the configured chain when the library has a configured zerostate.
- `payment_lane_depth`: 0 leaves locality enforcement to admission; a positive fixed shard depth checks that the source and destination belong to that lane.

It returns a scalar `NTXF` BOC. A chain with source-signed runs enabled rejects scalar native transfers; use the run method on that chain.

`native.createTransferRun` takes the same key, expiry, domain and lane arguments, plus `first_nonce` and 1–16 ordered `outputs`. Each output has `destination`, `amount` and `fee`. One Ed25519 signature authorizes the entire `NTRN` parent. Output `i` consumes nonce `first_nonce + i`.

Amounts must be positive, each amount plus fee and the aggregate run debit must fit uint64, and the interval must leave room to increment the final consumed nonce. A supplied positive lane depth is validated against the derived source address. Admission still checks the current protocol mode, lane/shard locality, balance, nonce reservations, signature, expiry and current account state. Construction does not promise acceptance.

The response contains `body` (BOC bytes), `hash` (the exact parent cell hash), `source`, `first_nonce`, `logical_count` and `valid_until`. The source address returned here equals `native.getAccountAddress` for the same key. Native signatures use the existing protocol domain separator and zerostate root hash; ordinary TON wallet signing is not used.

For example, after obtaining a key and a funded source:

```json
{
  "@type": "native.createTransferRun",
  "input_key": {
    "@type": "inputKeyRegular",
    "key": {"@type": "key", "public_key": "<tonlib public key>", "secret": "<base64 key-store secret>"},
    "local_password": "<base64 local password>"
  },
  "outputs": [
    {"@type": "native.transferOutput", "destination": {"@type": "accountAddress", "account_address": "0:<same-lane destination>"}, "amount": "1000", "fee": "0"}
  ],
  "first_nonce": "42",
  "valid_until": 2000000000,
  "chain_domain": "<base64 zerostate root hash>",
  "payment_lane_depth": 2
}
```

Read the current native nonce first and coordinate ownership when multiple clients use a source. The example expiry is illustrative; choose a future timestamp for the actual submission.

## Single submission and hashes

Submit the returned `body` with `raw.sendMessage` or `raw.sendMessageReturnHash`. Existing `raw.createAndSendMessage` constructs an ordinary TON external message; use native construction methods for native payments.

For a validated native `NTXF` or canonical `NTRN` envelope, `raw.sendMessageReturnHash` returns `hash_norm == hash == exact parent cell hash`. It does not normalize the envelope as an ordinary TON message and does not return individual output hashes. Different BOC transport encodings of the same cell have the same parent hash.

For ordinary external inbound TON messages, the existing normalized hash behavior is unchanged: normalization removes the external source, import fee and StateInit, retaining destination and body.

## Ordered batch submission

`raw.sendMessageBatch(bodies)` submits 1–1,024 BOC bodies, with a combined payload of at most 8 MiB. The method checks the aggregate size before decoding and preflights every BOC and message format. An empty, oversized or malformed local request fails before any RPC is sent. Each envelope remains intact; a native run counts as one body regardless of its logical output count.

Valid envelopes with incorrect signatures, stale/future nonces or insufficient balances are submitted for the server's per-item decision. The response is `raw.sendMessageBatchResult` with one `raw.sendMessageResult` for each original input position:

- `status`: 1 for admission success, 0 for rejection.
- `code`, `message`: the server's diagnostic fields; success uses 0 and an empty message. A rejected item can also have `code: 0` (for example, `status: 0, code: 0, message: "Wrong signature"`). Determine acceptance from `status`, never from `code` alone.
- `hash`, `hash_norm`: the original envelope's exact and normalized hashes, including for rejected items. Native values are identical as described above.

Duplicates retain their original positions. A batch can be partially accepted; it is not a transaction across all parents. An individual signed run retains its own atomic protocol semantics. A response with a mismatched count, missing status, unsupported status value or inconsistent success status fails the RPC result validation.

Transport, query-level and timeout failures remain whole-query errors; tonlib does not invent per-item success or rejection from them. Such errors may occur after partial admission. Preserve the original signed BOCs and reconcile their exact hashes/nonces before retrying or replacing them.

Admission success is not canonical inclusion. Capacity measurements must count proof-confirmed canonical logical transfers separately from RPCs, native parents, retries and admission acknowledgements. More clients or larger batches do not establish a TPS guarantee.

## Validation

Offline native-client tests cover uint64 boundaries, ordinary TON compatibility, independent message-hash normalization, signing domains, nonce/debit/lane bounds and ordered batch outcomes. The [bounded JSON-to-liteserver integration utility](../tonlib/test/native-client-integration.md) checks these public APIs against an isolated funded fixture, including partial batch rejection and exact canonical account deltas. It submits a finite number of native payments and makes no throughput claim.
