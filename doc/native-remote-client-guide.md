# One public validator and a remote native TPS generator

The original measurements used sidechain result commit `4b2034a8` and MyLocalTonDocker `44ffe58`. The export/import and remote-run scripts below are now maintained in MyLocalTonDocker. Their offline integration checks use simulated Docker; they do not establish new remote throughput measurements.

The existing `benchmark/run-native-connections-sweep.py --connections 10 50 100` is a **local Docker sweep**. It inspects a local `genesis`, shared volumes and process identity. Cloning it on server B does not enable remote mode; pointing `DOCKER_HOST` at A would run its generator on A. The underlying prebuilt generator already supports remote ADNL/TCP. Use the generator-only commands below on B. They retain proof-checked canonical counters but do not provide the local wrapper's full independent validator cleanup/resource acceptance report.

## Server A: bindings and native genesis

For a directly reachable liteserver and Session Stats, use these `.env` bindings:

```dotenv
LITESERVER_BIND_IP=0.0.0.0
SESSION_STATS_BIND_IP=0.0.0.0
SESSION_STATS_HTTP_PORT=18000
MANAGEMENT_BIND_IP=127.0.0.1
UI_BIND_IP=127.0.0.1
FILE_SERVER_BIND_IP=127.0.0.1
COMPOSE_PROFILES=
EXTERNAL_IP=
```

`127.0.0.1` publishes a port only on the Docker host; `0.0.0.0` publishes it on host interfaces. `EXTERNAL_IP` does not open a port. See [Docker port publishing](https://docs.docker.com/engine/network/port-publishing/).

**Why leave EXTERNAL_IP blank here?** This checkout also substitutes it for `INTERNAL_IP` during bootstrap and calls the validator console at that substituted address on port 40002. A public address with loopback-only management publishing can break fresh initialization. In an already initialized database the external config creation branch is skipped, so setting it afterward does not regenerate `external.global.config.json`. Export a separate public client config below; preserve the validator's local config. See `docker/scripts/start-genesis.sh:20`, `:492`, `:917` and `:959` in MyLocalTonDocker. This procedure covers client access to a single validator; public peer/validator topology is a separate setup.

Allow inbound **TCP 40004** for ADNL/TCP and **TCP 18000** for the dashboard in A's hosting firewall/security group, and forward them if A is behind NAT. If only B needs admission, allow B's public IP on 40004. Additional UDP consensus/DHT ports are not required by this remote client. Keep the file server private: the current genesis script copies validator/config/faucet private keys into `/usr/share/data`, which that service serves. Transfer only the selected public config and test source keys below.

For a **new checkout and fresh network**, `.env.physical` is the starting profile used by the measured desktop experiment:

```sh
# Fresh checkout only; preserve an existing deployment's .env and project name.
cp .env.physical .env
```

Edit the existing entries in `.env` rather than appending duplicate names. Apply the binding values above, retain a consistent Compose project name, and select the prebuilt matching images. The local tested image tags are shown below; they were not pushed to a registry merely by being committed to Git.

```dotenv
TON_BRANCH=cycle-clients-ed666c9a
SESSION_STATS_IMAGE=ghcr.io/neodix42/ton-session-stats:side
NATIVE_LOAD_IMAGE=mylocalton-native-load-generator:cycle-clients-ed666c9a-h2

NATIVE_TRANSFER_RUNS_ENABLED=1
NATIVE_PAYMENT_LANES_ENABLED=1
NATIVE_PAYMENT_LANE_DEPTH=2
ACTUAL_MIN_SPLIT=2
MIN_SPLIT=2
MAX_SPLIT=2
NATIVE_SPAM_GENESIS_DESTINATIONS=1
NATIVE_LOAD_SOURCES=24576
NATIVE_PAYMENT_LANE_WALLET_PARALLELISM=12
GENESIS_HEALTHCHECK_START_PERIOD=60m
TON_SIMPLEX_MAX_TPS=1
TON_NATIVE_CHECKPOINT_RETAIN_INGRESS=0
SPAM_RUN=0
NATIVE_SPAM_RUN=0
```

Four-lane native activation and funded accounts are **genesis requirements**, not changes to apply to an existing v14 chain. A correctly prepared, already-running four-lane chain can be reused. Check `/var/ton-work/db/native-spam/genesis.env` and its wallet manifest if unsure; verify that blocks are advancing. Do not reset an existing chain to follow this guide.

Adjust the copied `.env` CPU sets, quota and host database path to A's hardware before creating containers. For named database volumes, Session Stats' `TON_WORK_DOCKER_VOLUME` must identify A's actual genesis database volume; the sample project uses `mylocalton-desktop_ton-db-val0`. For a bind mount, use its absolute path as `TON_WORK_HOST_DIR` and clear `TON_WORK_DOCKER_VOLUME`. Retain the native Session Stats flags and `--session-logs` validator option in that profile.

Once the images are present, start only these two services:

```sh
docker compose --env-file .env up -d --no-build --pull never genesis
# Wait for genesis to be healthy and for last-block queries to advance.
docker compose --env-file .env --profile session-stats \
  up -d --no-deps --no-build --pull never session-stats

docker exec genesis lite-client -a 127.0.0.1:40004 \
  -p /var/ton-work/db/liteserver.pub -t 10 -c last
curl --fail http://127.0.0.1:18000/api/chart-config
```

A binding change on an existing container takes effect through Compose recreation, not plain `docker restart`; complete that work before timing. Keep the existing project name and mounts so the database is preserved. Open `http://SERVER_A_PUBLIC_IP:18000/` remotely. Public Session Stats does not require changing `UI_BIND_IP`.

## Export client materials on A

Three executable Bash scripts now replace the inline export/import/run examples. They live in **MyLocalTonDocker**, alongside the client preset:

| File | Purpose |
| --- | --- |
| [export-native-client.sh](../../MyLocalTonDocker/benchmark/remote/export-native-client.sh) | Ask for A's reachable address and export the selected client materials |
| [import-native-client.sh](../../MyLocalTonDocker/benchmark/remote/import-native-client.sh) | Verify and install the copied bundle on B; load its image if necessary |
| [run-remote-load.sh](../../MyLocalTonDocker/benchmark/remote/run-remote-load.sh) | Run the persistent ADNL connection sweep on B and save each arm's results |
| [native-remote-load.env](../../MyLocalTonDocker/benchmark/remote/native-remote-load.env) | The measured client preset included automatically by the exporter |

Server A needs Bash, Python 3 and Docker, a running prepared genesis, and the prebuilt generator image available locally. The image is resolved by immutable ID even when its archive is omitted; the exporter never builds or pulls one. The default image tag is `mylocalton-native-load-generator:cycle-clients-ed666c9a-h2`. These local image tags are not automatically published to a registry by committing code.

Run from the MyLocalTonDocker checkout on A:

```sh
bash benchmark/remote/export-native-client.sh
```

The script asks for:

- A's IPv4 address reachable from B and liteserver TCP port (default 40004).
- Source validator container (default `genesis`).
- Source account count (default 24,576) and first index (default 0).
- Existing generator image and a new export directory (default under your home directory).
- Whether to include the generator image archive (default yes).

For unattended operation, supply explicit options. Replace the documentation address below with A's actual public/LAN address:

```sh
bash benchmark/remote/export-native-client.sh \
  --non-interactive \
  --server-ip 203.0.113.10 --port 40004 \
  --container genesis --sources 24576 --source-offset 0 \
  --image mylocalton-native-load-generator:cycle-clients-ed666c9a-h2 \
  --include-image --output "$HOME/native-client-export"
```

Use `--no-image` if B already has the exact generator image. Existing output directories are refused; choose a new name for another export. Source count/offset select a contiguous funded range from A's existing lane manifest. The exporter discovers depth 1 or 2 from that manifest and adjusts the client preset accordingly; it does not create new accounts or change the chain. The reference 60k workload uses depth 2 and 24,576 sources.

The resulting **private directory** contains:

```text
native-client-export/
  external.global.config.json
  test-wallets.tar.gz
  remote-load.env
  import-native-client.sh
  run-remote-load.sh
  export-manifest.json
  generator-image.tar           # when image inclusion is enabled
```

Only the liteserver IP/port are rewritten in the exported public config. Its Ed25519 key and all chain hashes, including the zero-state signing domain, are preserved. The wallet archive includes the original public lane manifest and only `source-N.pk`, `source-N.pub`, `source-N.addr`, `dest-N.pub`, and `dest-N.addr` for the selected indices. It excludes destination private keys, validator/control keys and the validator database. SHA-256 checksums, sizes, image identity, endpoint and source-range metadata are recorded in `export-manifest.json`.

The exporter reads A's container; it does not expose ports, restart the validator or submit messages. The bundle contains funded source signing keys, so transfer the entire directory privately using SCP. Do not publish it through the file server or add it to Git.

## Copy and import on B

Copy the **whole directory**, including `run-remote-load.sh`, the importer and the environment file. There is no separate manual script/preset download step:

```sh
# On A; substitute B's SSH account and hostname.
scp -r "$HOME/native-client-export" user@SERVER_B:~/
```

B needs Linux, Bash, Python 3, the local Docker daemon, and standard `flock`/`timeout` commands. It does not need a validator or a repository clone. Use a generator image compatible with B's CPU architecture/instruction support. Do not point B's Docker context or `DOCKER_HOST` at A; the importer and runner require a local Unix-socket Docker endpoint.

On B, start the copied importer interactively:

```sh
bash "$HOME/native-client-export/import-native-client.sh"
```

It asks for the bundle directory and a new client installation directory, defaulting to the script's directory and `~/native-remote-client`. If the exact image is missing and an archive is included, it offers to load it. For unattended import, use this alternative:

```sh
bash "$HOME/native-client-export/import-native-client.sh" \
  --non-interactive \
  --bundle "$HOME/native-client-export" \
  --output "$HOME/native-remote-client" \
  --load-image
```

`--no-load-image` requires the pinned image to be preloaded on B. The importer verifies every bundled file checksum before using it, rejects unsafe/unexpected archive entries, checks the selected wallet range against its lane manifest, and installs into a new private directory. Existing client directories are preserved. It does not contact A or start load.

The installed layout is ready for the runner:

```text
native-remote-client/
  client-data/global.config.json
  client-data/wallets/...
  remote-load.env
  runtime-image-id.txt
  export-manifest.json
  import-native-client.sh
  run-remote-load.sh
```

An optional liteserver connectivity check on B uses the installed config and image:

```sh
cd "$HOME/native-remote-client"
image_id=$(cat runtime-image-id.txt)
docker run --rm --pull never --network host \
  --mount type=bind,src="$PWD/client-data",dst=/client,readonly \
  --entrypoint /usr/local/bin/lite-client "$image_id" \
  -C /client/global.config.json -t 10 -c last
```

B needs outbound TCP to A:40004 (or the exported port), with no inbound client port. The config supplies the liteserver public key. The generator uses persistent ADNL/TCP and the batch RPC, with one additional canonical-follower connection beyond the requested submission count.

## Run 10, 50 and 100 connections on B

The installed `run-remote-load.sh` is a real repository script. It runs only the generator; the existing local Python sweep remains separate.

```sh
cd "$HOME/native-remote-client"
bash run-remote-load.sh --connections 10 50 100
```

Omitting `--connections` uses the same 10/50/100 sequence. For a later independent single-count run or a different measurement duration:

```sh
bash run-remote-load.sh --connections 50 --duration 300
```

Run `bash run-remote-load.sh --help` for `--directory`, `--duration`, `--warmup`, `--drain`, `--cpus`, `--memory`, `--output` and `--image`. The default resource limits are 4 CPU equivalents and 8 GiB memory. Requested connection counts must fit the exported worker count (at least 6 for the reference preset) and must not exceed 256. Duplicate counts are rejected. For a different CPU-compatible prebuilt image, import/load it before running and explicitly select it with `--image`; the runner freezes its resolved ID across all arms.

Each arm uses unpaced bounded load, 60 seconds of warm-up, 180 seconds measured load and up to 180 seconds of drain, plus initial account/lane readiness work. The reference environment fixes six workers/signers, 24,576 sources, 16 logical transfers per signed run, a 64-parent batch cap, 20 ms coalescing and global initial/max admission windows of 32,768/65,536 logical transfers. Exporting fewer sources reduces worker/signer counts only when necessary. That smaller workload is not the reference capacity test.

Results are saved under a unique `remote-results/` subdirectory of the client installation, or the new directory provided with `--output`. The top-level `summary.json` contains every arm's status; each numbered connection directory retains `runtime-settings.json`, `runtime.env`, `generator.log`, `container.json`, `generator-final.json` and `summary.json`. Image identity, effective settings and public config are retained. Source keys are mounted read-only and are not copied into result reports. Exited workload containers remain available for inspection.

The runner serializes access to the installed client-data directory. Do not run another generator elsewhere using the same source keys; auto-nonce discovers canonical state but does not coordinate independent writers. Keep unrelated native traffic off A, keep A's images/process/configuration unchanged during the sequence, and synchronize A/B clocks through NTP. Chain TPS counts all native transfers in observed blocks, not only requests from B.

SIGINT/SIGTERM stops the runner-owned generator and preserves the partial result; a finite watchdog also bounds each arm. Inspect and reconcile an interrupted/incomplete run before reusing its source keys. The runner checks canonical proof/completion, zero final backlog, requested connection counts, signed-run density, batching and lane validity before starting another arm. Capacity-only rejections remain visible as `observation_only`; they do not discard valid throughput observations.

Use `canonical_chain_measure_avg_tps` in `generator-final.json` for block-time canonical throughput and `steady_mempool_accept_avg_tps` for admission. The runner's summary distinguishes `generator_capacity_eligible` from `observation_only`. It does not collect A's independent validator process/resource/pool-cleanup evidence, so neither label certifies the full local harness's strict capacity acceptance. Preserve A-side evidence separately and ensure offered load exceeds canonical TPS before making a capacity claim.

## Verified dashboard chart

The [2026-09-07 dashboard audit](session-stats-dashboard-audit-2026-09-07.md) verified the running image against all three completed tests. Use **“Canonical transactions per second” → “Workchain”**, with **Window size: 1m**. Its full-run totals exactly match the generator's independently proven 46,416,224 transfers. Minute peaks were 64,874 / 63,592 / 63,615 TPS for 10 / 50 / 100 connections; these are different windows from the measured-run averages.

The separate **“Canonical native transfers per second”** chart currently reports zero for NTRN: its importer assumes one logical transfer per physical accepted message. That bug is not fixed by this guide. Collation/validation service-rate charts are also not canonical chain throughput. The client transport does not change these counting rules.

The page defaults to the last two hours and does not auto-refresh. Reload or change the range during a test, allow importer delay, and select the historical interval when reviewing an older test. Use `BLOCK_APPLIED_transactions`, `mode=rate`, `window_size=60` for the verified API series.

## Bandwidth for the same approximately 60k TPS workload

60,000 TPS means **60,000 logical transfers/s**, packed into **3,750 new signed parents/s**. The measured generator repeats an identical destination/amount/fee 16 times within each parent; BOC cell deduplication makes that parent approximately **260 bytes**. Consequently, the fresh BOC payload alone is **7.8 Mbit/s B → A**.

Using the completed 10/50/100 sweep's whole-run retry and query proportions, estimated request traffic at 60k new logical TPS is **11.62 / 14.42 / 14.52 Mbit/s B → A** including TL/ADNL framing, before TCP/IP and retransmissions. At 100 connections there were approximately 1.784 physical attempts per fresh parent.

The canonical follower downloads complete blocks in the other direction. Candidate block payloads in these runs suggest roughly **33–35 Mbit/s A → B** at 60k, before proof/masterchain/account traffic and retries. This is a planning proxy, not measured canonical socket traffic. Actual 100-client Docker samples averaged 10.76 Mbit/s TX and 26.89 Mbit/s RX over 363 seconds including setup/warm-up/drain; they do not establish the steady-window WAN minimum.

An exact minimum has not been measured. For planning, avoid links below **100 Mbit/s full duplex** and prefer a dedicated **1 Gbit/s full-duplex link** for comfortable headroom and bursts. These are engineering allowances, not proof that a particular link will sustain 60k. On A, the larger requirement is upload toward B; on B it is download from A. Latency, packet loss and application credit windows also affect achieved TPS.

This estimate is workload-specific: 16 distinct outputs instead of repeated identical outputs produce approximately **1,018-byte parents**, raising fresh BOC request payload to about **30.5 Mbit/s before retries**. Sixty thousand independent scalar messages/s is also a different test from sixty thousand batched logical transfers/s.

Source references: `lite-client/native-load-generator.cpp:3725` (outputs), `crypto/vm/boc.cpp:223` (deduplication), `adnl/adnl-ext-connection.cpp:30` (framing), and `lite-client/native-load-generator.cpp:2603` (block downloads). The [saved desktop report](native-client-connections-2026-09-06.md) links original counters and classifications. A repeated remote benchmark with physical interface byte counters is needed to replace these estimates with a measured network requirement.
