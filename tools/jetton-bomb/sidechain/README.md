# Native-sidechain adapter

This copy comes from `ton-mesh/home-user/genesis/jetton-bomb` at revision
`bdeadcd57dfd2ec6f43d8f20e76ac8372675940f`. Existing secrets, network configs,
dependencies, history and executables were excluded. `UPSTREAM.json` records
the source and compiled-contract hashes.

Build the Go commands from the utility root:

```bash
make nativespam internalspam sidechaincheck
```

`sidechaincheck` detects the fork's execution mode from actual chain config.
Version 14+ basechain cannot process ordinary TVM/internal-message load;
`internalspam` can explicitly target masterchain `--workchain -1`.

`internalspam run` deploys `StaticTps`, confirms start, warms up, captures exact
account anchors, stops/drains and audits receipt histories. It reports useful
self-work separately from all target internal receipts, and wall throughput
separately from chain-timestamp throughput. The bundled contract covers
masterchain forwarding fees while retaining basechain message values.

See the [evaluation and local reproduction guide](../../../doc/jetton-bomb-sidechain-2026-10-05.md)
for measured results, the necessary absent-account serializer fix, runtime
provenance, and complete commands. The three local Python helpers build a test
image, start an isolated validator, and collect a bounded measurement.

`nativespam` sends this fork's source-signed NTRN payments directly over
ADNL/TCP. It requires funded native source keys, fixed native payment lanes,
and a matching chain signing domain. It supports 1..16 outputs per signed
request and uses `sendMessageBatch` for transport. No validator-local load
producer is used. The bounded run verifies account Merkle proofs, source
nonce increments, source debits, recipient credits, and final drain.

Native payments directly update balance/nonce state and create zero TON
internal-message cells. The runner reports committed **payments/s** and
**signed requests/s** separately; requests are inferred from atomic, fixed-size
nonce intervals. See the [native payment results and reproduction guide](../../../doc/jetton-bomb-native-2026-10-05.md).

```bash
python3 sidechain/start-local.py --name sidechain-native-bench --port 41007 \
  --env-file sidechain/native-local.env --native-sources 1024 \
  --output ../../build/benchmarks/native-bomb/materials
python3 sidechain/measure-native-local.py \
  --container sidechain-native-bench \
  --materials ../../build/benchmarks/native-bomb/materials \
  --outputs 16 --window 64 --output ../../build/benchmarks/native-bomb/native16.json
```

Run these from `tools/jetton-bomb` after building `nativespam`. Use `--outputs 1
--window 1024` with a new output filename for a single-payment comparison with
the same logical backlog bound. Each run stops issuing, retries the exact
outstanding signed requests, and waits for two unchanged advancing anchors.
Stop the isolated node afterward with `docker stop sidechain-native-bench`.
