# Remote native load: three-minute observations and ten-minute runner

Later observation: the [600-second run and admission-timeout fix](native-remote-drain-fix-2026-09-07.md) records a reported 63,831.51 TPS observation with an invalid 32-transfer drain tail, plus the corrected retry handling and subsequent 48-CPU client defaults.

The first user-run A/B sweep reported **74,512.27 canonical logical transfers/s** for 10 persistent submission connections, measured over 179 complete block-time seconds inside a 180-second offer window. The 50-connection container then exited abnormally, and the runner stopped before 100 connections. This is not yet a ten-minute sustained throughput result.

## Saved observations

Server B used pinned client image `sha256:2f39a329b4edacd7f014f3bb4004597cfd63d036502643784210a8ad6aa79d79` and retained artifacts under `/root/native-remote-client/remote-results/20260907T125604Z-396449`.

| Connections | User-reported outcome | Offered/admitted logical TPS | Canonical logical TPS | Measurement |
| ---: | --- | ---: | ---: | --- |
| 10 | Valid; generator capacity gates eligible | 74,648.89 | 74,512.27 | 180 offer seconds / 179 canonical seconds |
| 50 | Invalid: container did not exit cleanly | Unavailable | Unavailable | Full artifacts needed |
| 100 | Not started after the invalid 50-connection arm | — | — | — |

The full B-side artifacts and A-side process/resource/cleanup evidence have not been independently inspected. The 10-connection label is the runner's generator-side classification, not a complete validator capacity certification. Different hardware and measurement conditions prevent attributing a percentage improvement over the earlier desktop experiments to a specific code change.

A read-only query of A's dashboard returned these nonzero minute buckets for `BLOCK_APPLIED_transactions`, `mode=rate`, `window_size=60`:

| UTC minute, 2026-09-07 | Workchain logical TPS |
| --- | ---: |
| 12:58 | 11,195.73 |
| 12:59 | 81,022.93 |
| 13:00 | 81,081.87 |
| 13:01 | 73,851.73 |
| 13:02 | 58,505.87 |
| 13:05 | 33,344.53 |
| 13:06 | 66,637.33 |
| 13:07 | 74,958.40 |
| 13:08 | 64,663.73 |
| 13:09 | 44,381.87 |
| 13:10 | 35.47 |

These two short load regions match the supplied screenshot. The interval includes warm-up, measurement boundaries, drain and an invalid arm; the 81,081.87 minute peak is not a sustained ten-minute result. The 12:50–13:20 dashboard interval sums to 35,380,768 logical transactions, without a full reconciliation to B's proof-checked run totals. [Machine-readable observations](benchmarks/results/native-remote-short-runs-20260907.json) retain the exact API samples and reported summaries.

## Runner changes

MyLocalTonDocker's remote preset now requests 600 measured seconds. The host runner also applies a 600-second default floor to an older installed preset that still requests 180. Longer preset durations remain effective; an explicit `--duration` retains its exact meaning. The binary already supports 600 seconds, so this change requires only the B-side runner update and no TON image rebuild or validator restart.

Each setup has 60 seconds of warm-up, 600 measured seconds and up to 180 seconds of drain, plus readiness work. The three-count sequence has 30 measured minutes and intentional gaps between setups. Workload geometry and the default 4-CPU / 8-GiB generator limits remain unchanged. Longer duration supplies an observation window; it does not guarantee flat achieved TPS.

Progress appears every 30 seconds and is retained in `progress.jsonl`. The runner source and SHA-256 are saved with each sweep. Each arm records Docker exit/OOM/error/watchdog information in `execution.json`, and preserves a final generator record even when the process exits nonzero. Live rates remain provisional until final proof, completion, density and lane checks pass.

The old generic error combined restart, still-running state, OOM and nonzero exits before examining the final JSON. It cannot establish why the 50-connection arm failed. Native exit 2 can indicate unsettled drain; exit 3 can indicate follower/proof/correctness failure. `container.json`, `wait.log`, `generator.stderr.log` and any final JSON distinguish these cases. Exit 137 alone does not prove OOM.

The sequence still stops after an unclean or incomplete arm. Continuing automatically with unresolved source nonces would invalidate the next arm. No validator restart, image replacement, forced nonce reset or automatic memory increase is used to hide a failed run.

## Next measurement

Use the [guide's B-only runner update](native-remote-client-guide.md#update-an-already-imported-runner-on-b), resolve the previous failed arm from its saved evidence, then run:

```sh
cd "$HOME/native-remote-client"
bash run-remote-load.sh --connections 10 --duration 600
```

After a complete valid run, exercise all three setups:

```sh
bash run-remote-load.sh --connections 10 50 100 --duration 600
```

View **Canonical transactions per second → Workchain**, with a 1-minute window and a 15–20 minute display range around the single arm. Refresh after importer delay. For the full sweep, display the whole sequence and keep the per-arm warm-up/measurement/drain timestamps. Report per-minute behavior within the measurement alongside the final canonical average; do not infer stability from a peak or the requested duration.

The supplied QUIC `Unknown CID` messages do not diagnose this ADNL/TCP client's exit. Likewise, the Boolean work-time parser warning and missing consensus-parent warning do not establish the cause of the generator's process failure. The latter requires caution when interpreting dashboard ancestry without a full proof-result reconciliation.

The user's latest container listing also publishes management TCP 40002 and file-server TCP 8888. Restrict those endpoints before the next test; the remote load/dashboard procedure requires public TCP 40004/18000. Preserve the intended private management/file-server settings when applying deployment changes.

## Validation scope

All 37 remote integration tests passed after the final preset/documentation changes (58.558 seconds). The focused watchdog test also passed after adding an ambiguous exit-137 case: a killed `docker wait` reports unknown watchdog expiry rather than claiming a timeout or an OOM without evidence. All 28 shell examples checked across the updated remote documentation parse successfully.

These checks cover duration propagation (including upgrading an old imported preset), the scaled watchdog, progress, OOM/nonzero-exit evidence, retained final diagnostics and stopping before a subsequent arm on failure. They use simulated Docker and synthetic test keys. No new ten-minute A/B load was submitted by the assistant, and the old 50-connection failure remains unclassified pending its saved exit state and error logs.

## Subsequent 48-CPU scaling preparation

Implemented in [MyLocalTonDocker commit fd6f466](https://github.com/neodix42/mylocalton-docker/commit/fd6f466). The installed runner SHA-256 is `df90240dc0da067eac5a4f6adc8da3bd03315176af998e45eab959f703e7cc70`.

The user reported that A exposes 48 CPUs but uses only about 20, and that test traffic peaked around 50 Mbit/s on a 1 Gbit/s link. The old physical profile imposed an 18-CPU quota and an 18-CPU affinity mask, with 16 scheduler threads. The new `.env.physical` removes that affinity restriction and assigns 44 CPU equivalents, 40 scheduler threads, and 2 CPU equivalents for Session Stats. Eight native executor workers and the existing chain, queue, and checkpoint-retention settings remain fixed for the comparison. The historical profile is retained in MyLocalTonDocker commit `41c4107`.

B's standalone runner is independent of A's `.env.physical`. Its preset was already unpaced; its default CPU quota was four. The runner now accepts explicit worker, signer, initial-window, and maximum-window overrides, records their effective values, and checks their agreement with the final generator output. These options work with the already exported image. The [scaling procedure](native-remote-client-guide.md#increase-offered-load-on-b) tests CPU budget, signing parallelism, and admission windows separately before sweeping 50/100/256 connections. The reference defaults remain available as the control.

| Prepared comparison | Control | Treatment | Measured canonical TPS gain |
| --- | --- | --- | --- |
| A's CPU allocation and scheduler | 18 CPU / 16 threads / old affinity | 44 CPU / 40 threads / all available CPUs | Pending |
| B's CPU budget, if hardware permits | 4 CPU | 12 CPU | Pending |
| B's worker/signer parallelism | 6 / 6 | 12 / 12 | Pending |
| B's initial/maximum logical admission windows | 32,768 / 65,536 | 65,536 / 131,072 | Pending |

The larger maximum window gives roughly 81 sixteen-transfer parent messages of credit per connection at 100 connections, compared with roughly 40 previously. This permits a full 64-parent batch when sufficient work is ready; it is not a measured batching or throughput improvement. Connections still share one global window, and the current binary's limit remains 256. The in-flight and canonical backlog limits are unchanged.

Compose configuration validation confirmed the new quotas, empty affinity, 40 scheduler threads, and eight native workers without starting containers. All 41 runner integration tests passed with the new overrides, including invalid worker/window partitions, frozen image reuse, and failure-stop behavior. A subsequent review added a check that canonical backlog capacity covers every worker; its new regression and two related focused tests passed (the suite now contains 42 tests). The native batching entrypoint/configuration test also passed. These are offline correctness checks. No new A/B throughput result has been produced, so the earlier reported 74,512.27 TPS observation remains the recorded remote result, not a result of these settings.
