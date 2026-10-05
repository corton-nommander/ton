# Jetton-bomb native payment measurements — 5 October 2026

Jetton-bomb now includes an external Go native payment sender at
[`cmd/nativespam`](../tools/jetton-bomb/go-spam/cmd/nativespam). It signs real
`NTRN` envelopes and submits them through `liteServer.sendMessageBatch` to the
version-16 native basechain. The validator's internal load producers remain
disabled. Funded source accounts use their Ed25519 public keys as account IDs;
they do not deploy TVM wallets.

## Observed native message throughput

The primary runs used the same client binary and a stable four-leaf, depth-2
topology at every recorded phase. Each used 1,024 sources, ten seconds of warmup
and a 120-second measurement interval. A native message here is one signed
`NTRN` parent; its output payments are counted separately.

| Payments per signed message | Wall seconds | Completed signed messages | Native messages/s | Committed payments | Payments/s |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 120.0017 | 486,652 | **4,055.38** | 7,786,432 | **64,886.04** |
| 1 | 120.0021 | 424,344 | **3,536.14** | 424,344 | **3,536.14** |

Both runs passed the 1,024-pair account audit and fully drained their exact
offered cohorts. Including warmup and drain, the sixteen-output run committed
9,053,136 payments in 565,821 parents; the one-output run committed 675,607
payments/parents. These whole-run totals are not the table's measurement counts.
The one-output case still uses `NTRN`, not the legacy scalar `NTXF` format.

Submission was not error-free. Whole-command counters recorded 83,732 and
117,609 retried parents, 40,648 and 78,377 admission rejections, and one and eight
transport errors respectively. Retries reuse the original signed parent. Final
canonical nonce and balance reconciliation confirmed every issued parent;
admission acknowledgments and retries do not inflate the measured rates.
The isolated validator was stopped after both drains; its preserved container
exited with status 143 after SIGTERM, without an out-of-memory kill.

Chain timestamps spanned 120 and 121 seconds, producing 64,886.93 and 3,506.98
payments/s respectively. The table uses wall time. An earlier sixteen-output
pilot measured 70,990.42 payments/s while topology changed from two leaves to
four; it is retained as exploratory evidence and excluded from this comparison.
The early one-output pilot used the predecessor binary and is also excluded.
The final client explicitly requires the fixture's lane depth and rejects
topology changes during observation. Startup waits for the full configured
lane topology before exporting materials.

Measurement-only samples show the following resource use. Docker CPU percentage
is converted using 100% = one core; client CPU comes from its own cumulative
Linux process counters. Each primary arm has eleven resource samples.

| Payments/message | Validator mean cores | Sampled validator memory | External client mean cores | Sampled client RSS |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 5.83 | 3.00–3.76 GiB | 0.73 | 59.2–68.7 MiB |
| 1 | 5.11 | 3.74–4.17 GiB | 0.59 | 65.8–150.5 MiB |

Sampled validator CPU peaked at 8.98 and 10.05 cores within a 16-core quota.
The client uses eight sender workers, each awaiting one batch RPC at a time,
one liteserver connection and periodic proof reads to release nonce windows.
These are observable client limits; the experiment does not isolate signing,
network, admission or polling as the dominant bottleneck. CPU availability alone
does not establish additional throughput. The short sequential runs, changing
cache/database history and lack of repeated controls preclude a maximum-capacity
claim.

The [machine-readable summary](benchmarks/results/jetton-bomb-native-20261005.json)
retains exact masterchain/shard anchors, per-lane financial audit totals,
recomputed audit gates, binary/image/source hashes, resources and raw-file
SHA-256/size manifests. Full per-account snapshots and logs remain under
`build/benchmarks/jetton-bomb-native-20261005/`; private source seeds stay in
that ignored directory. The historical approximately 60k benchmark counted
logical native payment outputs. Its TPS unit is the payments/s column here,
and its different generator/profile is not a matched control for these runs.

## Measurement semantics

A **signed parent** is one submitted native `NTRN` message. It authorizes either
one or sixteen logical payments in these runs. Each payment consumes one source
nonce and credits its paired same-lane recipient. The runner reports committed
logical payment TPS and completed signed parent TPS separately. This native
workload creates **zero TON internal message cells**.

Every initial, measurement-start, measurement-end and final snapshot reads all
sources and recipients against one exact masterchain root and its authenticated
basechain shard roots. Native account Merkle inclusion, account IDs, state hashes
and dictionary balances are checked. The liteserver's masterchain root is trusted
locally; an independent consensus proof chain is not verified.

For each source, a committed nonce increase of `N` must match a balance decrease
of `N × (amount + fee)` and a recipient balance increase of `N × amount`.
Recipient nonces and all account flags must remain unchanged. Parent counts are
**derived from the audited nonce intervals divided by the fixed output count**,
using exclusive fixture accounts and atomic fixed-size runs; parent hashes are
not independently counted from canonical block batches. Admission acknowledgments,
transport attempts and retries are recorded separately and do not calculate TPS.

Warmup precedes the measured account interval. Offers stop afterward, outstanding
signed parents are reconciled, and every issued nonce must become canonical.
Successful drain also requires unchanged audited account state across two
advancing masterchain anchors. The final audit reconciles the complete offered
cohort; the throughput interval excludes warmup and drain.

## Local fixture and runtime

The fresh fixture funds 1,024 source accounts with one mock TON each and 1,024
distinct recipients with one nanotoken each. Four fixed depth-2 native lanes are
active. Each payment sends one nanotoken with zero fee. Both `NATIVE_SPAM_RUN`
and `SPAM_RUN` are zero, and there are no post-genesis topups. Only source signing
seeds and selected public keys are exported into the ignored benchmark directory.

The validator has a 16-CPU quota, a 12-GiB memory limit and a private writable
container overlay. Docker Desktop exposes 24 CPUs and approximately 80 GiB
of VM memory on a Ryzen 9 3900X host. Other local applications were running.
Host context was captured during testing, not at launch. The Go client runs
externally with eight sender workers,
submission batches of at most 64 parents and one local liteserver connection.
The nonce monitor refreshes all source proofs after each snapshot and a one-second
pause; snapshots use up to 32 concurrent account requests.

Both runs reserve up to 1,024 logical payments per source: 64 pending parents
with sixteen outputs, or 1,024 pending parents with one output. They use the same
funded fixture sequentially and discover current canonical nonces before sending.
This is a short workload comparison. Signing, serialization, transport, polling
and nonce-window behavior can constrain this client; these runs do not establish
the validator's maximum capacity.

## Validation

The complete normal Go suite passed, including native signing, compact account
decoding, financial audit and topology regressions. Both live benchmark runs
exercised Merkle proof verification. `go vet` passed for the three native packages.
Native signatures and parent
hashes also match independent C++ scalar/run construction vectors. No C++ load
generator or validator-internal producer submitted this benchmark traffic.

CLI/encoder race tests passed normally. The RPC race run initially aborted in
the SDK's unsafe TL serializer with Go `checkptr`; all three native packages
passed `-race` with `-gcflags=all=-d=checkptr=0`. This is a qualified race result,
not a claim that the SDK passed default `checkptr` instrumentation. Python
helpers passed compilation, bounded fixture-export/isolation checks and the
regression that waits through a partial two-leaf topology for all four leaves.

## Reproduce locally

Run from the repository root after building the local validator image as
documented in the [TVM compatibility report](jetton-bomb-sidechain-2026-10-05.md).

```bash
make -C tools/jetton-bomb nativespam sidechaincheck
python3 tools/jetton-bomb/sidechain/start-local.py \
  --name sidechain-native-bench --port 41007 \
  --env-file tools/jetton-bomb/sidechain/native-local.env \
  --native-sources 1024 \
  --output build/benchmarks/jetton-bomb-native-local/materials

python3 tools/jetton-bomb/sidechain/measure-native-local.py \
  --materials build/benchmarks/jetton-bomb-native-local/materials \
  --container sidechain-native-bench --sources 1024 \
  --outputs 16 --window 64 --duration 120s --warmup 10s \
  --drain-timeout 240s --cleanup-wait-seconds 300 \
  --output build/benchmarks/jetton-bomb-native-local/native16.json

python3 tools/jetton-bomb/sidechain/measure-native-local.py \
  --materials build/benchmarks/jetton-bomb-native-local/materials \
  --container sidechain-native-bench --sources 1024 \
  --outputs 1 --window 1024 --duration 120s --warmup 10s \
  --drain-timeout 240s --cleanup-wait-seconds 300 \
  --output build/benchmarks/jetton-bomb-native-local/native1.json

docker stop sidechain-native-bench
```

Wait for four depth-2 shard leaves before starting load; the Go runner rejects
incomplete topology. Use a new output filename for every run and use each source
range exclusively. SIGINT or SIGTERM stops new offers and requests bounded drain;
interrupted or incompletely reconciled results remain invalid. Logs, resource
telemetry and client/runtime/public-fixture provenance accompany the result JSON.
