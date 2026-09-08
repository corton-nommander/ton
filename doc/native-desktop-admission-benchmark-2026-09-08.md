# Desktop admission measurements — 2026-09-08

Status: three measurements completed; intermediate admission-window screen pending.

## Fixed workload and provenance

Fresh four-lane chain, 24,576 source/destination pairs, ten persistent ADNL/TCP
submission connections, six client workers/signers, 16 transfers per signed parent,
64-parent RPC batch cap, **20 ms coalescing**, 60 seconds warm-up and **600 seconds
measured**. The complete integer canonical timestamp window is 599 seconds.

AMD Ryzen 9 3900X, 12 cores / 24 threads, 128 GB installed RAM. Docker Desktop has
24 vCPUs and approximately 96 GiB RAM. Genesis receives a 16-vCPU quota and client
six vCPUs / 8 GiB, with disjoint VM CPU sets. Dashboard quota is one vCPU. VM CPU
sets do not establish physical-host SMT affinity. This same-host portable-image
measurement is not directly comparable with the remote 48-CPU server's ~50k TPS.

TON image revision: `64aabbff41d7a9d3e7d649994896baa9b27a69d0`, from the verified
AMD64 job of [GitHub run 34231105092](https://github.com/corton-nommander/ton/actions/runs/34231105092).
MyLocalTonDocker `.env.desktop` configuration commit: `e44ea67`.
Both derived images were built once before measurements. No image rebuild/pull
or validator restart occurred during the completed control.

Artifacts: `build/benchmarks/desktop-admission-20260908/` in the TON checkout,
including `images.json`, `run-arm.py`, per-arm strict-reuse bundles, raw generator
reports, getstats samples, Docker resource samples and `analysis.json`.

## Completed measurements

| Arm | Cache | Initial / max logical window | Offered TPS | Canonical native TPS | Drain / proof |
| --- | --- | --- | ---: | ---: | --- |
| `02-cache-off` | 0 | 32,768 / 65,536 | 61,170.6 | 61,150.0 | Passed; zero backlog/conflicts/follower errors |
| `03-cache-on` | 1 | 32,768 / 65,536 | 61,892.2 | **61,950.3** | Passed; zero backlog/conflicts/follower errors |
| `04-smaller-window` | 1 | 4,096 / 8,192 | 62,003.3 | 61,843.8 | Passed; zero backlog/conflicts/follower errors |

TPS counts logical native transfers, not independent signatures or general TVM
transactions. This control observed 36,628,864 canonical transfers in 599 complete
seconds. All offered work settled; generator exit was zero and no source exhausted
its retry horizon. The initial `01-control` attempt never emitted a load report:
it was stopped during topology readiness and is preserved as rejected setup evidence.

**Capacity qualification:** this image incorrectly considers an absent target
(`target_tps=0`) attained when evaluating its capacity flag. Raw flags are retained,
but the table reports sustained observed throughput. Control offered/canonical ratio
is only 1.00034, not the required 1.05 independent overdrive for an unpaced capacity
claim. Reporting correction `3dffcd38` passed 65 policy tests and a Release generator
build. It is not injected into the measured images; binaries remain fixed
throughout the comparison. Both arms are observed-throughput evidence.

Cache-on was 1.31% above the first control. One pair does not establish a repeatable
gain at this size. The 8,192-window arm was only 0.17% below cache-on, with lower latency and
no timeouts: `not_ready` responses fell from 624,249 to 333,984 (whole-run
physical response counts, including warm-up). The intermediate 16,384 ceiling
is the next screen; neither small difference establishes a repeatable TPS gain.

## Control diagnostics

Use sample intervals wholly inside measurement, not warm-up/drain or lifetime maxima:

- Admission snapshot changes rejected 18.55% of completed physical inputs.
- Average configuration extraction: 0.0117 ms; batch residence: 98.7 ms;
  shard wait: 51.9 ms; verification stage: 80.2 ms. Wall stages overlap.
- Average CPU equivalents: genesis 9.57, generator 0.78, dashboard 0.55.
- Block-signature verification created approximately 70 helper threads/second.
- Measured collated candidates averaged 6,032 transfers and 68.35 actual block
  bytes/transfer; mean collation work was 31.7 ms and external wait 111.7 ms.
- Maximum microbatch unique accounts: 64. All 27,226 measured staged-trie worker
  selections used one worker; none reached the 512-update parallel threshold.

The small config-extraction cost and high snapshot-retry fraction make bounded
exact-state refresh/revalidation a stronger next admission hypothesis than adding
client signing CPUs. More connections or higher credit limits require separate
measurements, since the live window was already below its configured ceiling.

## Bootstrap issue and workaround used

An idle maximum-TPS chain missed split windows because work-driven native collation
returned an idle result before materializing an empty split candidate. Masterchain
health alone did not establish four-lane readiness. After stopping the waiting
client (zero load reports), genesis temporarily used `TON_SIMPLEX_MAX_TPS=0`.
Four advancing leaves were verified at 14:25:30 UTC. Maximum-TPS mode was restored
before the measured control. No state or wallet volume was deleted.

Host `/proc`-based thread/cgroup figures from the legacy harness are excluded:
Docker Desktop container PIDs belong to the VM. Docker resource measurements and
validator-internal counters supply the reported resource evidence.
