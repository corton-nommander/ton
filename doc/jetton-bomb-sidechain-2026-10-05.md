# Jetton-bomb compatibility and internal-message measurements — 5 October 2026

Jetton-bomb is copied into [`tools/jetton-bomb`](../tools/jetton-bomb), with an
explicit workchain-aware internal-message runner. It can exercise the current
sidechain's **masterchain TVM**. Its ordinary wallet/jetton traffic cannot run
on the optimized version-16 **basechain**. The historical approximately 60k TPS
measurements counted logical native payment outputs, a different execution path.

## Observed performance

Both measured runs used protocol version 16, capabilities 3310 (`0xCEE`), four
depth-2 native basechain leaves, and one validator. Traffic stayed in masterchain.
Each contract requested 3,000 self-work messages/s. Deployment and ten seconds of
warmup preceded measurement; stop, drain and transaction auditing followed it.

| Contracts | Wall seconds | Useful self-work messages | Useful messages/s | Target internal receipts/s | Aborted receipts |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 120.0098 | 117,995 | **983.21** | 1,312.13 | 0 |
| 16 | 120.0365 | 149,673 | **1,246.90** | 1,343.98 | 0 |

“Useful” means the contract's committed `totalTxCount` increase. Target internal
receipts count all inbound internal transactions at these contracts, including
returned bounces and ignored work from older epochs. The 4-contract run audited
157,469 receipts, including 39,458 bounces; the 16-contract run audited 161,327,
including 11,590 bounces. Transactions at the shared ping sink and other accounts
are outside these counts. These are self-messaging results, not jetton transfers,
whole-network TPS, cross-shard TPS or native payment TPS.

Each start/end snapshot reads every target at the **same exact masterchain root**.
The audit walks complete account histories backwards from the end hash/LT to the
start hash/LT, verifies predecessor links, rejects forked or missing boundaries,
and excludes deployment, warmup and stop/drain. Both runs passed the audit and
confirmed stopped contracts with unchanged transaction LTs across two advancing
masterchain anchors. RPC acceptance counters do not contribute to throughput.

Chain timestamps spanned 120 and 119 seconds respectively, giving 983.29 and
1,257.76 useful messages/s. The table uses wall time. A ten-second smoke test had
only seven seconds between chain timestamps, so its higher chain-time rate was
not used as a sustained result. The proof policy trusts the local liteserver's
masterchain/account snapshots; transaction hash links are checked, but independent
consensus proof verification is not performed.

The validator had a 16-CPU quota and 12 GiB memory limit in Docker Desktop
(24 CPUs, 80 GiB VM memory), on a Ryzen 9 3900X host. Its database used a fresh
container overlay. Other local applications were running. Measurement-only
resource samples averaged 0.96 and 0.76 CPU cores, with sampled memory below
1 GiB. These short, unpaired runs establish observed workload throughput, **not
maximum capacity**. More contracts changed the work/bounce mix; total target
receipt throughput changed only slightly. Simplex also logged already-finalized
candidate-slot messages; no accepted target transaction aborted.

The [machine-readable result](benchmarks/results/jetton-bomb-sidechain-20261005.json)
retains exact anchors, receipts, resources, source/image/binary hashes and artifact
hashes. Raw files remain under `build/benchmarks/jetton-bomb-20261005/`. Private
test-wallet seeds are confined to that ignored directory. All benchmark contracts
were stopped and drained, and the isolated validator was stopped after testing.

## Compatibility boundary

The collator and validator enable native execution whenever `workchain == 0 &&
global_version >= 14`, in [`collator-impl.h`](../validator/impl/collator-impl.h)
and [`validate-query.hpp`](../validator/impl/validate-query.hpp). In that mode,
[`collator.cpp`](../validator/impl/collator.cpp) skips inbound internal queues and
rejects ordinary externals/generated messages. Corresponding validation rejects
inbound/outbound message descriptors and requires unchanged processed-message
state. Fees, endpoints and ordinary batching settings cannot enable this traffic.

`sidechaincheck` reads actual ConfigParam 8, checks configured/observed zerostate
identity, and records exact masterchain/shard identities. It exits **3** for
ordinary TVM on the optimized basechain and **0** for masterchain. The original
LS-backed `spam` and `deploy` commands now perform the same check before running.
Legacy HTTP modes retain their upstream behavior; use the LS path for this fork.

The historical [native reference](native-master-reference-2026-09-11.md) records
61,380.86 and 62,537.54 canonical logical outputs/s in separate ten-minute runs.
Up to sixteen native outputs share one signed parent. These operations use the
native balance/nonce state engine and do not execute the TVM/internal-message
cascade exercised here.

Porting the original jetton pool to masterchain would additionally require
coordinated changes to Go `walletspam.New`/`jetton.WalletAddress`, Tolk
`WalletSpam.siblingStateInit` and `sharding.tolk`, followed by regenerated embedded
BOCs and address/parity vectors. They currently derive basechain addresses.
The new runner uses jetton-bomb's existing `StaticTps` contract instead.

Supporting jettons/internal messages on the native basechain requires coordinated
protocol work for TVM execution, account handling, message queues and validation.
Alternatively a separate version-13 TVM basechain can run ordinary contracts, or
the generator can be rewritten around NTRN payments. Neither alternative measures
version-16 basechain internal-message throughput.

## Two issues found and fixed

The original image rejected the first masterchain deployment at seqno 1124 with
an incorrect old account hash. The fork uses `account_none$00`, but
`ShardAccount::extract_account_state` synthesized a legacy one-bit empty account
for absent entries. Collation produced a two-bit old state, so validation rejected
every deployment candidate. [`block-parse.cpp`](../crypto/block/block-parse.cpp)
now synthesizes the canonical two-bit value. The regression in
[`test-cells.cpp`](../crypto/test/test-cells.cpp) compares generated TL-B
serialization, `Record::reset`, new accounts in both workchains and preservation
of explicit existing states. All 41 `test-cells` tests passed.

After that fix, the original 0.003-TON self-message value was insufficient for
masterchain forwarding fees. Its ignore-errors send mode silently skipped work
messages; a smoke run produced only bounces and was correctly marked invalid.
`StaticTps` now attaches 1 TON to **masterchain** self-messages while retaining
0.003 TON in basechain. This float returns to the same account; fees reduce its
balance. Local benchmarks fund each contract with 1,000,000 mock TON. Bounded
masterchain/basechain sandbox tests both execute useful self-work and confirm
the expected values. The complete Go suite also passed.

The measured image contains the locally rebuilt Release/native validator with
this one-line serializer fix. Its host loader/shared libraries are bundled for
compatibility with the existing Ubuntu-22 genesis image. Other genesis tools
come from that existing image. This is a recorded local test runtime, not a new
production publication; use the repository Dockerfile for deployment images.

## Reproduce locally

Run from the sidechain repository root. The existing genesis image and configured
CMake build are prerequisites; the helper never pulls an image or removes an
existing chain volume.

```bash
cmake --build build --target validator-engine test-cells --parallel 4
build/test-cells
make -C tools/jetton-bomb internalspam sidechaincheck

python3 tools/jetton-bomb/sidechain/build-local-image.py \
  --validator build/validator-engine/validator-engine
python3 tools/jetton-bomb/sidechain/start-local.py \
  --name sidechain-jetton-bench \
  --output build/benchmarks/jetton-bomb-local/materials

# Expected exit 3: ordinary TVM messages cannot execute in this basechain.
tools/jetton-bomb/go-spam/build/sidechaincheck \
  --ls-config build/benchmarks/jetton-bomb-local/materials/local-config.json \
  --workchain 0

python3 tools/jetton-bomb/sidechain/measure-local.py \
  --materials build/benchmarks/jetton-bomb-local/materials \
  --container sidechain-jetton-bench --contracts 16 --nonce 1000 \
  --duration 120s --warmup 10s \
  --output build/benchmarks/jetton-bomb-local/sixteen-contracts.json

docker stop sidechain-jetton-bench
```

Use a fresh nonce range for each run. `internalspam` also has `plan`, `status` and
`stop` phases; preserve owner, workchain, contract count, nonce and code when
addressing an existing run. SIGINT/SIGTERM request stop/drain, and failed
confirmation, incomplete audit or unsuccessful drain invalidates the result.
The runner writes JSON, a log and telemetry alongside the requested output.
`--reuse` on the startup helper exports materials from an already-running,
matching isolated container; it validates image, environment, mounts and port.

Go deploy/load uses embedded or bundled compiled code and needs no Node/Acton at
runtime. To change `StaticTps`, install its TS dependencies and compile the
contract, then replace `sidechain/assets/StaticTps.compiled.json`; the current
source/artifact provenance is in `sidechain/UPSTREAM.json`.
