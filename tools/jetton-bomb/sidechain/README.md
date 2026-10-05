# Native-sidechain adapter

This copy comes from `ton-mesh/home-user/genesis/jetton-bomb` at revision
`bdeadcd57dfd2ec6f43d8f20e76ac8372675940f`. Existing secrets, network configs,
dependencies, history and executables were excluded. `UPSTREAM.json` records
the source and compiled-contract hashes.

Build the Go commands from the utility root:

```bash
make internalspam sidechaincheck
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
