# Native clients and persistent liteserver connection sweep — 2026-09-06

The requested native tonlib client APIs are implemented and verified against the real local liteserver. The 10 → 50 → 100 persistent ADNL/TCP connection sweep completed on one unchanged validator. The highest observed canonical rate was **63,433.12 logical transfers/s at 50 connections**. The **100-connection result, 60,250.28 transfers/s, passed the capacity gates** with 4.92% more offered load than canonical throughput.

[Machine-readable results and saved configurations](benchmarks/results/native-client-connections-20260906.json) preserve every arm, original acceptance decisions, final counters, launch plan, raw-summary hashes, image IDs and validator identity. Full raw bundles remain in `/home/neodix/gitProjects/MyLocalTonDocker/benchmark-results/client-connections-20260906-sweep2/`.

## Implemented client support

- Existing ADNL/TCP transport and `liteServer.sendMessage` remain the submission path. The existing forked lite-client `sendfile` accepts prepared native BOCs. This benchmark uses the persistent `native-load-generator` ADNL clients.
- `native.getAccountAddress` derives the native basechain address from the Ed25519 public key. `native.getAccountState` decodes proof-checked compact accounts and preserves the entire uint64 balance/nonce range as decimal strings. Ordinary account responses gain the native variant; legacy signed-int64 balance responses reject overflow explicitly.
- `native.createTransfer` and `native.createTransferRun` construct and sign NTXF/NTRN BOCs with explicit source nonce, expiry, chain domain and optional lane validation. They preserve existing key-store handling and do not deploy TVM wallets or reserve nonces automatically.
- `raw.sendMessageReturnHash` returns exact native parent hashes, with `hash_norm == hash`. Ordinary TON normalization is preserved.
- `raw.sendMessageBatch` accepts 1–1,024 intact BOCs, at most 8 MiB in total, and returns ordered per-parent status/code/message/hashes. Local malformed requests fail before submission; server semantic rejections remain per item. **Acceptance is determined by `status`; a rejected message may have numeric `code:0`.**
- The load generator has an optional global initial congestion window and explicit paced/unpaced telemetry. The new sweep runner accepts one connection count or a list, freezes images/process identity across arms, and retains rejected observations without relaxing gates. A separate `NATIVE_LOAD_IMAGE` override permits prebuilt generator changes while preserving the validator image and configuration.

See [native tonlib API documentation](native-tonlib-client-api.md), the [live JSON test](../tonlib/test/native-client-integration.md), and [sweep usage](../../MyLocalTonDocker/benchmark/native-connections-sweep.md).

## Validation and commits

58 generator policy tests and all 27 tonlib offline tests passed; the 12 native tests were rerun after checking the real server's zero-code rejection behavior. All 16 sweep tests and the existing harness reporting suite passed. The [live JSON integration result](benchmarks/results/native-client-json-integration-20260906.json) verifies native/raw/generic account reads, scalar/run construction, independent parent hashes, single submission, ordered `[1,0,1]` batch results, and exact canonical balance/nonce changes.

The first live test stopped on an incorrect test assertion requiring a nonzero rejection code; the server correctly returned `status:0, code:0, message:"Wrong signature"`. Its 64 accepted logical transfers were independently reconciled before the corrected full test passed. In total, 128 tiny logical transfers were canonically resolved before TPS timing.

| Repository | Commit | Change |
| --- | --- | --- |
| Sidechain | `8ede7f38` | Comparable global starting windows and explicit load-mode telemetry |
| Sidechain | `ed666c9a` | Native tonlib APIs, regression coverage and live integration utility |
| Sidechain | `8d343fc4` | Live-verified rejection-status test/documentation correction |
| Harness | `45ad2c5` | Persistent connection sweep and initial-window wiring |
| Harness | `44ffe58` | Actual `<arg>` CLI-help detection, failed-report handling and generator-image override |

A first sweep launch failed before offering load because the capability detector did not recognize the real `--adaptive-initial-cwnd<arg>` help format. Its zero-record bundle is preserved as `client-connections-20260906-sweep1`. The corrected generator-only image was built and checked before the complete sweep; no validator restart or image build occurred inside the successful 10/50/100 series.

## Fixed experiment conditions

This was a **fresh four-lane network**, because prior Docker containers and named volumes were absent. Existing images remained available. Provisioning created 24,576 funded sources and 24,576 same-lane destinations without deleting existing state. These results do not isolate a code-level gain against the historical approximately 50k runs on another chain/database history.

One validator and its embedded liteserver ran on the Ryzen 9 3900X desktop through Docker Desktop: 24 vCPUs available, an 18-vCPU validator quota, a 4-vCPU generator quota, six generator workers and six signers. The canonical follower uses one additional generator connection; monitoring queries are separate from the requested submission connection count. No transport or external listener exposure was changed.

| Control | Value in every arm |
| --- | ---: |
| Target TPS | 0: unpaced submission with bounded credits/backlogs |
| Warm-up / measured window / maximum drain | 60 / 180 / 180 seconds |
| Logical transfers per fresh signed parent | 16 |
| Global initial / maximum adaptive window | 32,768 / 65,536 logical transfers |
| Hard inflight / canonical backlog / source backlog | 262,144 / 2,097,120 / 128 logical transfers |
| Physical batch limit / coalescing deadline | 64 parents / 20 ms |
| Concurrent admission query cap | 64 per submission client |

The validator used `ghcr.io/neodix42/mylocalton-docker:cycle-clients-ed666c9a`; the generator used `mylocalton-native-load-generator:cycle-clients-ed666c9a-h2`. All three arms retained validator container `22fa35704e789c3de0cc0017e6c63e99667780e59ac35f5b1721f38fe11be68a`, daemon PID `12136`, kernel start ticks `141510`, and restart count zero. Exact full image IDs are saved in the result JSON. Source and derived images were prebuilt, with strict reuse enabled throughout.

## Results

TPS below counts **logical transfers**, with 16 transfers per signed external parent. Batch RPC counts are a separate unit. Offered/admission rates use the 180-second measured cohort; canonical rates use 179 fully contained integer block-time seconds. This boundary difference and carried pending work allow canonical window TPS to exceed measured offered TPS slightly.

| Submission connections | Offered logical TPS | Admitted logical TPS | Canonical logical TPS | Capacity classification |
| ---: | ---: | ---: | ---: | --- |
| 10 | 62,528.09 | 62,528.09 | 62,800.80 | Observation only |
| 50 | 62,955.73 | 62,955.73 | **63,433.12** | Observation only |
| 100 | 63,214.49 | 63,214.49 | **60,250.28** | **Capacity eligible** |

Every arm passed canonical proof correctness, completion, ingress, exact signed-run density, lane balance, requested batching mode, strict image/process reuse and validator cleanup. Every final canonical backlog was zero, with zero measured canonical-backpressure fraction. The 10- and 50-client chain-capacity decisions remain rejected because their measured offered rates were below canonical throughput. The 100-client arm had 4.92% offered overdrive and passed both ingress and chain-capacity gates.

The separate source-reproducibility flag remains false solely because the unchanged Session Stats image lacks a source-revision label. Its exact image ID is recorded and remained fixed. No gate was relaxed.

The descriptive 50-versus-10 difference is **+1.01%**; 100 versus 50 is **−5.02%**. These are observations from an ascending sweep, without randomized order or repetition. They do not establish a repeatable connection-count gain, a new code-level gain, an optimum, or remote-client capacity.

## Transport and resource observations

[Transport audit](benchmarks/results/native-client-connections-20260906-transport.json) retains the underlying counts. The following are **whole-run** metrics, including warm-up, measurement, retries and drain; they are not measured-window RPC rates.

| Metric | 10 clients | 50 clients | 100 clients |
| --- | ---: | ---: | ---: |
| Mean physical parents per batch | 15.86 | 20.21 | 17.05 |
| Admission RPCs per 1,000 unique admitted logical transfers | 6.03 | 5.57 | 6.60 |
| Physical parent attempt amplification | 1.42× | 1.78× | 1.78× |
| Retry schedules | 409,006 | 752,613 | 756,940 |
| Admission RTT p50 bucket upper bound, ms | 200 | 500 | 500 |
| Admission RTT p95 bucket upper bound, ms | 500 | 500 | 1,000 |

Retries were predominantly not-ready responses and eventually resolved by canonical proof. RTT histograms in these artifacts contain 16 logical-child samples per physical parent attempt; the older textual per-message label is imprecise. Fixed 16-transfer density preserves percentile comparisons within this sweep. Bucket upper bounds are not exact latency quantiles.

At 100 clients, equal division of the 65,536-credit global maximum allows only 40–42 whole parents per client, consistent with the observed maximum batch of 42. Both per-client AIMD trajectories and batching efficiency vary with connection count. The 50-client run achieved better RPC amortization than the 100-client run; this is diagnostic evidence, without establishing a sole cause for the throughput difference.

Existing Docker samples whose timestamps fall inside the measured window averaged approximately 10.42 / 10.04 / 9.71 validator-container CPU cores and 0.795 / 0.881 / 0.874 generator cores for 10 / 50 / 100 clients. Exact sampling-interval boundaries are unavailable, so these are approximate sample means, not exact measured-window CPU integrals. Host/network, memory stalls, serialized stages and sampling effects remain relevant.

## Retained configuration and next experiment

The implemented client code and prebuilt validator image remain in place. The result JSON saves **50 connections as the highest observed configuration** and **100 connections as the capacity-qualified configuration**. The sweep runner preserves its full 10/50/100 default sequence; historical production defaults were not promoted from an unqualified observation.

Reproduce the full sweep against the same prepared network from the harness directory:

```sh
cd /home/neodix/gitProjects/MyLocalTonDocker
env TON_BRANCH=cycle-clients-ed666c9a   NATIVE_LOAD_IMAGE=mylocalton-native-load-generator:cycle-clients-ed666c9a-h2   TON_SIMPLEX_MAX_TPS=1 TON_NATIVE_CHECKPOINT_RETAIN_INGRESS=0   python3 benchmark/run-native-connections-sweep.py     --connections 10 50 100 --duration 180     --output benchmark-results/connections-repeat-unique
```

Use a new output directory each time. `--connections 50` or `--connections 100` selects one saved configuration. The next useful comparison is a declared balanced-order 50/100 repetition, followed by a separate window/batching experiment that gives each of 100 clients enough credit for a full 64-parent batch. Preserve finite backlog bounds, exact image/process reuse and all existing acceptance gates. Separately measure retry/snapshot-lag causes before changing admission policy. A remote-server client test is required for WAN capacity claims.

After this sweep the validator is healthy with the same process identity, the generator exited successfully, all submitted benchmark work is canonically resolved, and no TPS measurement remains running.
