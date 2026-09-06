# Tonlib native JSON integration test

Run `tonlib/test/native-client-integration.py` only after the new `tonlibjson` library and local native lane network are ready. The two consecutive selected source indices and their destinations must be funded, native, independent, and unused by another generator during this test. Existing volumes/accounts are preserved; all signing keys stay in an in-memory tonlib keystore. The JSON report omits private keys, imported key secrets, request bodies, and signed BOCs.

Example (replace paths with the freshly prepared fixture paths):

```sh
python3 tonlib/test/native-client-integration.py \
  --library build/tonlib/libtonlibjson.so \
  --config /path/to/local-global.config.json \
  --wallet-dir /path/to/funded-native-wallets \
  --source-index 0 --lane-depth 2 --logical-count 16 \
  --output build/benchmarks/client-connections-20260906/tonlib-integration-1.json
```

The script imports `source-N.pk` (32 raw bytes), validates the derived native address against `source-N.pub`, and uses `dest-N.pub`. Public account IDs/lane placement are checked before submission. It exercises `native.getAccountAddress`, `native.getAccountState`, `getAccountState`, `raw.getAccountState`, `native.createTransfer` (construct/hash only), `native.createTransferRun`, `raw.sendMessage`, `raw.sendMessageReturnHash`, and `raw.sendMessageBatch` through the actual JSON C ABI and liteserver.

It rejects wrong configured domain, overflowing nonce, expired request, wrong workchain/lane, and empty batch before submitting four valid parents (64 logical transfers at the default quantum). An independently parsed/rehashed BOC verifies each returned native hash. The batch contains `[valid, invalid-signature, valid]` in that order, with the last valid parent using the second source to avoid a nonce dependency on the invalid parent. The invalid signature is changed in its leaf while preserving valid BOC structure/CRC. Server statuses must remain `[1,0,1]` and identify signature rejection. The rejected item may have `code: 0`; the test determines acceptance from `status` and checks its signature diagnostic without requiring a nonzero numeric code. Source nonce/balance and destination credit are polled until the exact canonical deltas appear; the invalid parent must consume no nonce or funds.

At defaults source N spends 48 × (1 amount + 1 fee) = 96 nanotokens and its destination receives 48; source N+1 spends 16 × (1 + 1) = 32 and its destination receives 16. This deliberately tiny correctness workload is not a capacity/TPS benchmark. The report records library/config SHA256, account block IDs and request timings. It fails closed on malformed/missing responses, mismatched hashes/statuses, unexpected account deltas, or deadlines; a failed report retains `all_gates_passed:false`. Run with a new output filename for every attempt. If an attempt partly submits before failing, a retry reads the new canonical nonce and does not reset any account.

This utility is opt-in and is not registered in the offline CTest suite. It requires the real local validator, funded fixture directory and exclusive account use described above.
