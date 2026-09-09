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

For a **new checkout and fresh network**, `.env.physical` now targets a 48-logical-CPU server A with load generated remotely. Its CPU allocation is a new, unmeasured scaling treatment; the older desktop measurements used a different allocation:

```sh
# Fresh checkout only; preserve an existing deployment's .env and project name.
cp .env.physical .env
```

Edit the existing entries in `.env` rather than appending duplicate names. Apply the binding values above, retain a consistent Compose project name, and use the registry-backed image settings below. If A already has the older `cycle-clients-*` entries, replace those entries in its existing `.env`; do not overwrite the deployment file or database settings.

```dotenv
TON_BRANCH=master
SESSION_STATS_IMAGE=ghcr.io/neodix42/ton-session-stats:side
NATIVE_LOAD_IMAGE=mylocalton-native-load-generator:master
TON_BUILD_PULL=true

NATIVE_TRANSFER_RUNS_ENABLED=1
NATIVE_PAYMENT_LANES_ENABLED=1
NATIVE_PAYMENT_LANE_DEPTH=3
ACTUAL_MIN_SPLIT=3
MIN_SPLIT=3
MAX_SPLIT=3
NATIVE_LOAD_PAYMENT_LANE_DEPTH=3
NATIVE_SPAM_GENESIS_DESTINATIONS=1
NATIVE_LOAD_SOURCES=24576
NATIVE_PAYMENT_LANE_WALLET_PARALLELISM=12
NATIVE_PAYMENT_LANE_WALLET_RETRIES=256
GENESIS_HEALTHCHECK_START_PERIOD=60m
TON_SIMPLEX_MAX_TPS=1
TON_NATIVE_CHECKPOINT_RETAIN_INGRESS=0
SPAM_RUN=0
NATIVE_SPAM_RUN=0
```

Eight-lane native activation and matching funded accounts are **genesis requirements** for a fresh physical-server deployment. The updated MyLocalTonDocker helpers support fixed depths 1, 2 and 3 (two, four and eight lanes). The 24,576 sources remain evenly distributed, giving 3,072 sources per lane at depth 3. Topology readiness allows 1,800 seconds for three masterchain-anchored split rounds after bootstrap; this is outside measured load time.

Keep an existing four-lane deployment's depth/split values at 2. To test eight lanes, use separate genesis state and matching new wallets; changing these environment variables cannot convert an existing chain by restart. Check `/var/ton-work/db/native-spam/genesis.env` and its wallet manifest before selecting a topology. Do not reset an existing chain to follow this guide.

The exporter derives lane count and both client depth settings from that manifest, so old two/four-lane exports remain supported while a fresh physical-server genesis exports eight-lane tests automatically. Update the MyLocalTonDocker checkout and rebuild the derived genesis/client wrappers through the launcher below; an old wrapper image still rejects depth 3. The TON binary already supports this depth, so these changes do not require a new consensus feature or a TON image workflow change.

Adjust the copied `.env` CPU sets, quota and host database path to A's hardware before creating containers. For named database volumes, Session Stats' `TON_WORK_DOCKER_VOLUME` must identify A's actual genesis database volume; the sample project uses `mylocalton-desktop_ton-db-val0`. For a bind mount, use its absolute path as `TON_WORK_HOST_DIR` and clear `TON_WORK_DOCKER_VOLUME`. Retain the native Session Stats flags and `--session-logs` validator option in that profile.

The Git branch `native-payment-lanes-step6` selects MyLocalTonDocker's scripts. It does **not** select or download the TON binaries: `.env` `TON_BRANCH` is an image tag. The images have separate roles:

| Setting | Role |
| --- | --- |
| `TON_IMAGE:TON_BRANCH` | TON base image containing the validator, lite-client and native-load-generator binaries |
| `MLT_IMAGE:TON_BRANCH` | Derived genesis image with MyLocalTonDocker's initialization scripts |
| `NATIVE_LOAD_IMAGE` | Derived client image with the load-generator entrypoint and lane helper |
| `SESSION_STATS_IMAGE` | Separate dashboard image |

The [TON image workflow](https://github.com/corton-nommander/ton/actions/workflows/docker-ubuntu-branch-image.yml) builds `master` on pushes and manual runs. It verifies package write access before compilation, builds portable `linux/amd64` and `linux/arm64` binaries, and checks their revision and native-client commands. Only after both architectures pass does it promote `ghcr.io/corton-nommander/ton:master` and `:latest`. Immutable `sha-<full Git SHA>` tags remain available for a specific revision. Check the workflow's **successful** run and image publication before first startup; a Git push or a running/failed job is not a published image.

Server A obtains the base directly from GHCR. No desktop image export is needed. From the updated MyLocalTonDocker checkout, use the host launcher:

```sh
bash start-native-genesis.sh --env-file .env
```

This always pulls the configured `TON_IMAGE:TON_BRANCH`, resolves its registry digest and full source revision, and locally builds **both** genesis and the native client from that same digest. The build wrappers do not compile TON. It records the base and derived image IDs in `.native-images.json`, then starts only genesis with the prepared local image. A pull/build/provenance failure stops the launcher; it does not silently fall back to an old image.

Running this launcher again intentionally checks for an updated published TON image and may recreate genesis through normal Compose startup when its image changes. It preserves the configured database volumes and project name. Complete it before measuring TPS; do not run it during a connection sweep. `master` means the latest successfully published master revision, which may lag an in-progress or failed source build. The public portable image is not the historical desktop image, so the recorded 60k results must be measured again on A/B.

To prepare images without starting or recreating containers:

```sh
bash prepare-native-images.sh --env-file .env
```

Once genesis is healthy and blocks advance, start Session Stats:

```sh
# Run last twice, allowing time for new blocks; confirm the block ID advances.
docker exec genesis lite-client -a 127.0.0.1:40004 \
  -p /var/ton-work/db/liteserver.pub -t 10 -c last

docker compose --env-file .env --profile session-stats pull session-stats
docker compose --env-file .env --profile session-stats \
  up -d --no-deps --no-build --pull never session-stats
curl --fail http://127.0.0.1:18000/api/chart-config
```

`docker compose up --no-build --pull never genesis` alone deliberately reuses local images. Use `start-native-genesis.sh` when launching/updating from the current registry version. A plain `docker restart` cannot fetch a new image.

A binding change on an existing container takes effect through Compose recreation, not plain `docker restart`; complete that work before timing. Keep the existing project name and mounts so the database is preserved. Open `http://SERVER_A_PUBLIC_IP:18000/` remotely. Public Session Stats does not require changing `UI_BIND_IP`.

### Scale an existing 48-CPU server A

The previous `.env.physical` limited genesis to **18 CPU equivalents**, the affinity mask `0-8,12-20`, and **16 scheduler threads**. That mask exposes only 18 logical CPUs even on a 48-CPU host. The updated profile uses:

```dotenv
GENESIS_CPU_QUOTE=44
VALIDATOR_CPU_QUOTE=44
GENESIS_CPUSET=
SESSION_STATS_CPU_QUOTE=2
SESSION_STATS_CPUSET=
NATIVE_LOAD_CPUSET=
```

In the existing `CUSTOM_PARAMETERS` entry, change only `--threads 16` to `--threads 40`. Keep `TON_NATIVE_EXECUTOR_THREADS=8` for this comparison: ingress verifier actors share the scheduler, but native execution helpers can also create threads. The empty affinity settings allow scheduling across all CPUs available to Docker. The 44-CPU limit leaves four CPU equivalents outside the validator's budget; it does not reserve particular cores or guarantee full utilization. See [Docker CPU constraints](https://docs.docker.com/engine/containers/resource_constraints/#cpu).

Pulling the repository does **not** update an existing deployment's `.env`. Save it before editing, then replace the resource entries above in that file. Preserve its project name, mounts, public bindings, image settings, and genesis parameters. Use `nproc` and `lscpu -e=CPU,NODE,SOCKET,CORE` on A to confirm the host exposes the intended 48 logical CPUs. This profile assumes the generator runs on B; a simultaneously running local generator would share A's resources.

After any active load run has finished and drained, apply the updated `.env` using the **currently running genesis image ID**. Run this from MyLocalTonDocker on A:

```sh
bash -s <<'SH'
set -eu
umask 077
genesis_image_id=$(docker inspect --format '{{.Image}}' genesis)
resource_override=$(mktemp)
trap 'rm -f "$resource_override"' EXIT
python3 - "$genesis_image_id" "$resource_override" <<'PY'
import json, pathlib, re, sys
assert re.fullmatch(r'sha256:[0-9a-f]{64}', sys.argv[1]), 'Expected immutable image ID'
pathlib.Path(sys.argv[2]).write_text(json.dumps({'services': {'genesis': {'image': sys.argv[1]}}}))
PY
docker compose --env-file .env -f docker-compose.yaml -f "$resource_override" \
  up -d --no-deps --no-build --pull never --force-recreate genesis
docker inspect --format 'image={{.Image}} nano_cpus={{.HostConfig.NanoCpus}} cpuset={{.HostConfig.CpusetCpus}}' genesis
SH
```

This deliberately restarts genesis once to apply the resource/thread settings. Compose preserves its mounted database; no new zero state or wallet export is needed. `nano_cpus=44000000000` and an empty `cpuset` confirm the new CPU limit and affinity. Wait for healthy status and advancing blocks before measuring again. [Compose recreation behavior](https://docs.docker.com/reference/cli/docker/compose/up/) documents the volume preservation and `--no-build`/`--pull never` options.

Apply the Session Stats quota separately, also reusing its prepared image:

```sh
docker compose --env-file .env --profile session-stats \
  up -d --no-deps --no-build --pull never --force-recreate session-stats
```

Recreating only Session Stats also resolves an old stopped stats container referencing a deleted Compose network. Neither this command nor the export script starts the load generator. Keep all images and validator settings fixed throughout each subsequent measurement sequence. The historical 18-CPU profile is available in MyLocalTonDocker at `41c4107:.env.physical` for a controlled comparison; do not replace the deployment's entire `.env` with that historical file.

## Export client materials on A

Three executable Bash scripts now replace the inline export/import/run examples. They live in **MyLocalTonDocker**, alongside the client preset:

| File | Purpose |
| --- | --- |
| [export-native-client.sh](../../MyLocalTonDocker/benchmark/remote/export-native-client.sh) | Ask for A's reachable address and export the selected client materials |
| [import-native-client.sh](../../MyLocalTonDocker/benchmark/remote/import-native-client.sh) | Verify and install the copied bundle on B; load its image if necessary |
| [run-remote-load.sh](../../MyLocalTonDocker/benchmark/remote/run-remote-load.sh) | Run the persistent ADNL connection sweep on B and save each arm's results |
| [native-remote-load.env](../../MyLocalTonDocker/benchmark/remote/native-remote-load.env) | The measured client preset included automatically by the exporter |

Server A needs Bash, Python 3, Docker with Compose, and a running prepared genesis. The exporter reads Compose's resolved `native-load-generator` image from the deployment `.env`. By default it invokes `prepare-native-images.sh` to pull the current configured TON base and build only the client from its immutable digest. No image-name prompt or desktop image transfer is needed.

The pulled TON source revision must match the running genesis image. If master has advanced since A was launched, export stops before building a mismatched client and asks you to run `start-native-genesis.sh`, wait for healthy/advancing blocks, and export again. Export itself never recreates genesis or starts traffic. The resulting client image is frozen by ID in the bundle and across B's complete connection sweep.

Run from the MyLocalTonDocker checkout on A:

```sh
bash benchmark/remote/export-native-client.sh
```

The script asks for:

- A's IPv4 address reachable from B and liteserver TCP port (default 40004).
- Source validator container (default `genesis`).
- Source account count (default 24,576) and first index (default 0).
- A new export directory (default under your home directory).
- Whether to include the generator image archive (default yes).

For unattended operation, supply explicit options. Replace the documentation address below with A's actual public/LAN address:

```sh
bash benchmark/remote/export-native-client.sh \
  --non-interactive \
  --server-ip 203.0.113.10 --port 40004 \
  --container genesis --sources 24576 --source-offset 0 \
  --include-image --output "$HOME/native-client-export"
```

Use `--env-file /path/to/deployment.env` for another Compose environment file. `--build-image` explicitly selects the default registry preparation. `--no-build-image` opts into strict reuse of the existing configured client, skipping all pulls/builds. An explicit `--image PREBUILT_IMAGE` also bypasses registry preparation and requires that image locally. These explicit reuse modes are for an already prepared benchmark; ordinary deployment/export follows the registry by default. Complete preparation before the benchmark, then reuse the exported immutable image throughout all runs.

Use `--no-image` if B already has the exact generator image. Existing output directories are refused; choose a new name for another export. Source count/offset select a contiguous funded range from A's existing lane manifest. The exporter discovers depth 1, 2 or 3 from that manifest and adjusts the client preset accordingly; it does not create new accounts or change the chain. Before publishing a depth-3 bundle, it checks that the pinned generator image contains eight-lane initialization helpers. The historical reference 60k workload uses depth 2 and 24,576 sources.

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

B needs Linux, Bash, Python 3, the local Docker daemon, and standard `flock`/`timeout` commands (typically supplied by `util-linux` and `coreutils`). Docker alone is not the complete dependency list. B can start with **no Docker images**: leave image inclusion enabled during export, and the importer will load `generator-image.tar` without a registry pull, build, Compose installation or repository clone. Use a generator image compatible with B's CPU architecture/instruction support. Do not point B's Docker context or `DOCKER_HOST` at A; the importer and runner require a local Unix-socket Docker endpoint.

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
bash run-remote-load.sh --connections 10 50 100 --duration 600
```

Omitting `--connections` now selects one 10-connection arm. The updated runner defaults to **at least 600 measured seconds per count**, even when an older installed `remote-load.env` still says 180. Longer environment durations remain effective; an explicit `--duration` selects the requested duration, including a shorter diagnostic run. For a later independent single-count run:

```sh
bash run-remote-load.sh --connections 10 --duration 600
```

Run `bash run-remote-load.sh --help` for `--directory`, `--duration`, `--warmup`, `--drain`, `--profile`, `--source-policy`, `--cpus`, `--memory`, `--workers`, `--signers`, `--initial-cwnd`, `--max-cwnd`, `--output` and `--image`. The default `server48` profile uses 40 CPU equivalents, 48 GiB memory, ten workers and 32 signers, including when an older imported environment still specifies six workers/signers. `--profile preset` retains the imported worker/signer values and uses the earlier 4-CPU/8-GiB resource defaults; explicit CLI overrides win. Requested connection counts must fit the effective worker count (ten for the full server48 preset) and must not exceed 1024 with the updated binary. Duplicate counts are rejected. For a different CPU-compatible prebuilt image, import/load it before running and explicitly select it with `--image`; the runner freezes its resolved ID across all arms.

Each arm uses unpaced bounded load, 60 seconds of warm-up, **600 seconds measured load** and up to 180 seconds of drain, plus initial account/lane readiness work. A full three-count sweep therefore contains 30 measured minutes and takes longer than 33 minutes including warm-up, readiness and drain. It produces three separate load periods, with intentional gaps between counts. The server48 environment uses ten workers, 32 signers, 24,576 sources, 16 logical transfers per signed run, a 64-parent batch cap, 20 ms coalescing and global initial/max admission windows of 32,768/65,536 logical transfers. Exporting fewer sources reduces worker/signer counts only when necessary. That smaller workload is not the reference capacity test.

Duration extends the observation window; it does not guarantee a flat TPS line or fix a failed generator. Use one 10-connection arm first to observe an uninterrupted ten-minute measurement, then run the full sweep after resolving any previous failed arm. The native binary already supports this duration, so updating the host runner requires no new TON image, validator restart, wallet export, or image transfer.

Results are saved under a unique `remote-results/` subdirectory of the client installation, or the new directory provided with `--output`. The top-level `summary.json` contains every attempted arm's status. The runner saves its own source and SHA-256 alongside the image identity, effective settings and public config. Each numbered connection directory retains `runtime-settings.json`, `runtime.env`, `generator.log`, `generator.stderr.log`, `container.json`, `execution.json` and `summary.json`, plus `generator-final.json` whenever a final record was emitted, including on a nonzero exit. Source keys are mounted read-only and are not copied into result reports. Exited workload containers remain available for inspection.

Periodic console progress reports phase, elapsed/measurement time and provisional TPS/backlog counters. `canonical_chain_measure_planned_window_avg_tps` divides observed transfers by the entire planned canonical window (typically 599 seconds), including while measurement is still running. Its gradual rise is not a live throughput ramp. Raw native field names remain in `generator.log`; the final proof/completion checks remain authoritative. The watchdog grows with the selected duration, warm-up, ramp, drain and readiness budgets.

The runner serializes access to the installed client-data directory. Do not run another generator elsewhere using the same source keys; auto-nonce discovers canonical state but does not coordinate independent writers. Keep unrelated native traffic off A, keep A's images/process/configuration unchanged during the sequence, and synchronize A/B clocks through NTP. Chain TPS counts all native transfers in observed blocks, not only requests from B.

SIGINT/SIGTERM stops the runner-owned generator and preserves the partial result; a finite watchdog also bounds each arm. Inspect and reconcile an interrupted/incomplete run before reusing its source keys. The runner checks canonical proof/completion, zero final backlog, requested connection counts, signed-run density, batching and lane validity before starting another arm. Capacity-only rejections remain visible as `observation_only`; they do not discard valid throughput observations. A failed 50-connection arm therefore stops before 100, with the exact exit/OOM/watchdog state and any final generator failure reasons retained. It never silently skips an incomplete arm and reuses its keys.

For an older result that says only `container did not exit cleanly`, inspect the saved files on B (substitute the actual arm directory):

```sh
cd "$HOME/native-remote-client/remote-results/RUN/02-50-connections"
python3 -c 'import json; d=json.load(open("container.json"))[0]; print(json.dumps({"State":d.get("State"),"RestartCount":d.get("RestartCount")},indent=2))'
cat wait.log
tail -n 60 generator.stderr.log
tail -n 5 generator.log
```

Exit 2 can indicate unsettled drain; exit 3 can indicate canonical follower/proof/correctness failure. Use the final record and stderr to identify the actual reason. Exit 137 alone does not prove OOM; inspect `State.OOMKilled`. CPU and memory limits are explicit knobs for B, not established explanations for an unexplained exit.

Use `canonical_chain_measure_avg_tps` in `generator-final.json` for block-time canonical throughput and `steady_mempool_accept_avg_tps` for admission. The runner's summary distinguishes `generator_capacity_eligible` from `observation_only`. It does not collect A's independent validator process/resource/pool-cleanup evidence, so neither label certifies the full local harness's strict capacity acceptance. Preserve A-side evidence separately and ensure offered load exceeds canonical TPS before making a capacity claim.

### Two remote generators against one genesis

One liteserver can accept persistent connections from both B and C. A second
generator can increase offered load and bandwidth, but canonical TPS increases
only if the first generator was constraining supply and A has remaining capacity.
The earlier plateau across connection counts does not establish a network limit.
Each generator also downloads and verifies the chain independently, so a second
observer adds liteserver block/proof work even if accepted TPS stays unchanged.

Stop and completely drain the old full-range generator before preparing this
test. Never use its same source keys concurrently on B and C: the client-data
lock is local to each installation, and auto-nonce does not coordinate writers
across hosts. The existing 24,576 funded sources can be split into two disjoint
groups; no genesis reset or lane change is needed. On an eight-lane chain, each
12,288-source group contains 1,536 sources per lane. The exporter detects the
actual manifest depth.

From A's MyLocalTonDocker checkout, export the two groups using the same already
prepared generator image. Keep that image unchanged between exports; use new
output directory names if these already exist:

```sh
bash benchmark/remote/export-native-client.sh \
  --non-interactive --server-ip 2.59.170.242 --container genesis \
  --sources 12288 --source-offset 0 --no-build-image \
  --output "$HOME/native-client-B"

bash benchmark/remote/export-native-client.sh \
  --non-interactive --server-ip 2.59.170.242 --container genesis \
  --sources 12288 --source-offset 12288 --no-build-image \
  --output "$HOME/native-client-C"
```

The commands use A's deployment `.env`; add `--env-file PATH` if it differs.
Each bundle includes the image pinned by immutable ID. Copy B's bundle only to
B and C's only to C, then import each into a new client directory using the
normal importer. Source offsets are export options, not remote-runner arguments.
On each client, after import:

```sh
cd "$HOME/native-remote-client"
bash run-remote-load.sh --connections 10 --duration 900 --warmup 60 \
  --submit-coalesce-ms 20
```

Start both close together with synchronized host clocks. Readiness and warmup
are independent; there is no synchronized measurement-start barrier. Confirm
at least 600 seconds of overlapping measured load in the final timestamps and
analyze that common interval on A. A 900-second run provides room for startup
skew, but does not guarantee the overlap.

**Do not add the two canonical TPS readings.** Both followers count the same
whole-chain transfers; their source-cohort proof accounting is separate. Use one
canonical chain measurement for the common window and sum only distinct offered
or admitted logical transfers over that same window. Retry attempts are not new
offered transfers. Separate per-client capacity labels compare that client's own
offer rate with whole-chain TPS and may correctly remain `observation_only`.
Complete proof/drain checks on both clients and retain A's matching profile.

Treat combined client credits as an explicit test setting: two default clients
double the initial/maximum aggregate congestion windows to 65,536/131,072.
Compare equal combined source/connection/credit budgets when isolating the effect
of distributing generation; increase total budgets separately only if their
counters show a limit. If more unique offers increase RTT, backlog and retries
without increasing canonical TPS, reduce excess load and optimize A's measured
admission, decoding, execution or persistence bottleneck. If canonical TPS rises,
the previous single generator or its supply path was limiting the observed rate.
Higher network use alone is not evidence of improvement.

### Update an already imported runner on B

After the previous runner has exited, update only the installed host script below. The original exported directory remains unchanged because its checksums cover the original scripts. The existing pinned generator image and installed wallets are reused. For a fixed update, set `runner_ref` to the reviewed MyLocalTonDocker commit instead of the maintained branch shown.

```sh
bash -s <<'SH'
set -eu
umask 077
runner_ref=native-payment-lanes-step6
client_dir="$HOME/native-remote-client"
runner_download=$(mktemp)
trap 'rm -f "$runner_download"' EXIT
curl --fail --location --retry 3 "https://raw.githubusercontent.com/neodix42/mylocalton-docker/$runner_ref/benchmark/remote/run-remote-load.sh" --output "$runner_download"
bash -n "$runner_download"
cp -p "$client_dir/run-remote-load.sh" "$client_dir/run-remote-load.sh.before-update-$(date -u +%Y%m%dT%H%M%SZ)"
install -m 700 "$runner_download" "$client_dir/run-remote-load.sh"
SH
```

The [remote run observations and duration change](native-remote-long-runs-2026-09-07.md) distinguish the reported 74.5k TPS three-minute result from the ten-minute measurements that still need to be run.

### Increase offered load on B

A's `.env.physical` does not configure the standalone generator on B. The user has confirmed that **B also has 48 CPUs and 256 GB RAM**, so the new default `server48` profile uses:

| Setting | Previous default | New default |
| --- | ---: | ---: |
| Container CPU budget | 4 | 40 |
| Container memory ceiling | 8 GiB | 48 GiB |
| Worker actors | 6 | 10 |
| Signing actors | 6 | 32 |
| Scheduler threads (native binary) | 13 | 43 |
| Initial / maximum logical admission window | 32,768 / 65,536 | 32,768 / 65,536 |

Ten workers distribute 10/50/100/300/500 connections evenly. The CPU allocation leaves room for the host, Docker, and monitoring. The memory value is a ceiling, not a reservation or instruction to fill RAM. The preset already uses `NATIVE_LOAD_TARGET_TPS=0`, meaning bounded **unpaced** load. The failed run's sample had a congestion window near 8,174, no acknowledgments clipped by the 65,536 ceiling, and no query-credit stalls; it does not justify automatically doubling admission or backlog limits.

The profile applies even to an old imported environment when the host script is updated; it leaves that original file unchanged. Each arm records the effective profile/settings. Use `--profile preset` for the imported worker/signer settings and earlier 4-CPU/8-GiB budget; explicit CPU, memory, worker, signer, and window arguments override either profile. On smaller B hosts, select `preset` and size its resource/parallelism overrides explicitly.

**The resource controls require only a host-script update; the admission-timeout fix below requires a new generator binary/image.** Complete that image upgrade before another capacity run. Then start with one valid ten-minute arm:

```sh
cd "$HOME/native-remote-client-fixed"
bash run-remote-load.sh --connections 10 --duration 600
```

After that arm drains and passes the final checks:

```sh
bash run-remote-load.sh --connections 10 50 100 --duration 600
```

For an explicit old-resource control on the same fixed image and A configuration, including with a newly exported preset:

```sh
bash run-remote-load.sh --profile preset --connections 10 --duration 600 \
  --cpus 4 --memory 8g --workers 6 --signers 6 \
  --initial-cwnd 32768 --max-cwnd 65536
```

After obtaining repeatable valid results, the larger connection sweep remains available:

```sh
bash run-remote-load.sh --connections 50 100 300 500 --duration 600
```

Only if counters show the admission window now limits useful traffic, compare a larger window at a fixed connection count:

```sh
bash run-remote-load.sh --connections 100 --duration 600 \
  --initial-cwnd 65536 --max-cwnd 131072
```

Windows count **logical transfers across all workers/connections**, not bytes or per-connection messages. Explicit values must be positive, the initial window must not exceed the maximum, and the maximum must fit the exported in-flight budget (262,144 in the reference preset). The runner validates that worker/client partitions can dispatch complete signed runs. The canonical backlog budget remains 2,097,120 logical transfers. Treat each command as a separate experiment and inspect its final result before reusing its sources. CPU and signing changes are new treatments, not measured TPS improvements.

The updated native binary and runner accept at most **1024 submission connections**. Values above 256 require the new published/exported generator image; the runner checks the binary capability before starting any arm. More connections divide the same global admission budget: at 65,536 logical transfers, 100 connections average about 40 sixteen-transfer parent messages of credit each, while 500 would average only eight. Increasing signing capacity or useful outstanding work can supply more load without adding that RPC overhead.

The observed 50 Mbit/s on a 1 Gbit/s link does not establish unused validator capacity. At the reported 74.5k logical TPS, sixteen-transfer signed runs represent only about 4,660 fresh parent messages per second. Batch packing and retries affect wire traffic. Compare offered, admitted, and proven canonical TPS, signing rate, CPU usage/throttling on both hosts, admission-window limits, query stalls, follower lag, and final backlog. Retain the configuration with the highest **repeatable, valid ten-minute canonical TPS**, rerun its control, and save the image IDs, A's resource settings, and the runner's result directory. No TPS gain from the 44-CPU or larger-window treatments has been measured yet.

### Retry-horizon recovery and connection counts above 256

For a new sweep that must attempt later setups after an unresolved arm, use `--source-policy isolated --connections 100 300 500`. Each setup receives a disjoint, lane-balanced 8,192-source partition from a 24,576-source export. Failed arms stay invalid; later arms become observation-only if earlier traffic remains unresolved. This does not reconcile source ranges from a previous failed sweep. See the [isolation and comparison limits](native-remote-retry-recovery-2026-09-07.md#deployment-and-measurement).

The subsequent 50-connection run used the timeout fix (`resigned=0`), but one exhausted source still left 128 logical transfers unresolved. The [follow-up correction and evidence](native-remote-retry-recovery-2026-09-07.md) preserve pending signed parents for bounded reconciliation after new issuance is quarantined, and shorten retries for explicit signed-run snapshot/revision races. It requires another generator image update, using the upgrade sequence below after publication. A recovered, fully proven run can continue to the next connection count but remains ineligible for capacity claims when retry exhaustion occurred. A genuinely incomplete drain still stops reuse of those sources.

### Upgrade the image for the admission-timeout drain fix

The September 7 ten-minute run exited with code 2 after a complete measurement and a 180-second drain that remained at **32 unresolved logical transfers**. It was not OOM, a Docker restart, or a watchdog kill. The client incorrectly recognized `external message admission deadline expired` as transfer expiry and re-signed a still-valid parent. The sample's 39 timeouts, 39 expiry counts, and 39 re-signings match this bug. The fix is in TON commit **`a1e4c988`**; request timeouts now retry the exact signed bytes. The [failure report](native-remote-drain-fix-2026-09-07.md) separates this confirmed bug from the still-unconfirmed attribution of the final 32 transfers.

Updating only `run-remote-load.sh` cannot fix that binary. After the TON image workflow successfully publishes a revision containing `a1e4c988`, update A from GHCR and export a new matching client image. Preserve the old failed run and stop all generators before this maintenance step:

```sh
# On A, from the existing MyLocalTonDocker checkout.
git pull --ff-only
bash start-native-genesis.sh --env-file .env
```

Use the existing `.env`, project name and database mounts. This intentionally upgrades/recreates genesis once between tests and retains the chain/accounts. It also removes the old process's in-memory ingress state, which can contain reservations from the failed run. Do not delete volumes, regenerate wallets, or treat the old incomplete run as valid. Wait for healthy genesis and advancing blocks, then export to a **new** directory:

```sh
bash benchmark/remote/export-native-client.sh
```

The exporter verifies that the new client matches running genesis's revision and includes the prebuilt client image. Transfer that new bundle from A to B as described above. On B, import it into a new client directory so the failed run's artifacts remain available:

```sh
# Replace the bundle directory with the newly copied export.
bash "$HOME/native-client-export-NEW/import-native-client.sh" \
  --output "$HOME/native-remote-client-fixed"
cd "$HOME/native-remote-client-fixed"
bash run-remote-load.sh --connections 10 --duration 600
```

The first announcement should show `profile=server48`, 40 CPUs/48g, ten workers and 32 signers. Its pinned image ID must differ from the old failing image `sha256:e016826b0bb5c4e11d01dd7b31cf5aae3843c9066720f2ad5affecfdcc59b731`. Check the exported receipt's source revision as well; a different image ID alone does not prove the fix is present. Keep the new image fixed across the next sweep. The original 180-second drain limit and failure gates remain in force.

If a drain still fails, inspect `summary.json`'s compact `generator_diagnostics`, `generator-final.json`, and `generator.stderr.log`. The fixed binary logs at most eight unresolved source samples per worker after the final proof poll, with source indices, nonce positions, disabled state and pending-parent state. Private keys and message bodies are not printed.

## Verified dashboard chart

The [2026-09-07 dashboard audit](session-stats-dashboard-audit-2026-09-07.md) verified the running image against all three completed tests. Use **“Canonical transactions per second” → “Workchain”**, with **Window size: 1m**. Its full-run totals exactly match the generator's independently proven 46,416,224 transfers. Minute peaks were 64,874 / 63,592 / 63,615 TPS for 10 / 50 / 100 connections; these are different windows from the measured-run averages.

The separate **“Canonical native transfers per second”** chart currently reports zero for NTRN: its importer assumes one logical transfer per physical accepted message. That bug is not fixed by this guide. Collation/validation service-rate charts are also not canonical chain throughput. The client transport does not change these counting rules.

The page defaults to the last two hours and does not auto-refresh. Reload or change the range during a test, allow importer delay, and select the historical interval when reviewing an older test. Use `BLOCK_APPLIED_transactions`, `mode=rate`, `window_size=60` for the verified API series.

For a ten-minute stability observation, choose a 15–20 minute interval around that arm and keep the 1-minute window. A 600-second measurement plus warm-up should occupy a visibly longer interval than the old three-minute measurement; gaps between separate setups are expected. Preserve the exact generator measurement timestamps and compare only buckets within that window. A high minute peak, or a longer configured duration alone, does not prove stable TPS for ten minutes.

## Bandwidth for the same approximately 60k TPS workload

60,000 TPS means **60,000 logical transfers/s**, packed into **3,750 new signed parents/s**. The measured generator repeats an identical destination/amount/fee 16 times within each parent; BOC cell deduplication makes that parent approximately **260 bytes**. Consequently, the fresh BOC payload alone is **7.8 Mbit/s B → A**.

Using the completed 10/50/100 sweep's whole-run retry and query proportions, estimated request traffic at 60k new logical TPS is **11.62 / 14.42 / 14.52 Mbit/s B → A** including TL/ADNL framing, before TCP/IP and retransmissions. At 100 connections there were approximately 1.784 physical attempts per fresh parent.

The canonical follower downloads complete blocks in the other direction. Candidate block payloads in these runs suggest roughly **33–35 Mbit/s A → B** at 60k, before proof/masterchain/account traffic and retries. This is a planning proxy, not measured canonical socket traffic. Actual 100-client Docker samples averaged 10.76 Mbit/s TX and 26.89 Mbit/s RX over 363 seconds including setup/warm-up/drain; they do not establish the steady-window WAN minimum.

An exact minimum has not been measured. For planning, avoid links below **100 Mbit/s full duplex** and prefer a dedicated **1 Gbit/s full-duplex link** for comfortable headroom and bursts. These are engineering allowances, not proof that a particular link will sustain 60k. On A, the larger requirement is upload toward B; on B it is download from A. Latency, packet loss and application credit windows also affect achieved TPS.

This estimate is workload-specific: 16 distinct outputs instead of repeated identical outputs produce approximately **1,018-byte parents**, raising fresh BOC request payload to about **30.5 Mbit/s before retries**. Sixty thousand independent scalar messages/s is also a different test from sixty thousand batched logical transfers/s.

Source references: `lite-client/native-load-generator.cpp:3725` (outputs), `crypto/vm/boc.cpp:223` (deduplication), `adnl/adnl-ext-connection.cpp:30` (framing), and `lite-client/native-load-generator.cpp:2603` (block downloads). The [saved desktop report](native-client-connections-2026-09-06.md) links original counters and classifications. A repeated remote benchmark with physical interface byte counters is needed to replace these estimates with a measured network requirement.


## Admission profiling after the eight-lane plateau

The [September 9 admission plan](native-bottleneck-action-plan-2026-09-09.md) is
approved; desktop implementation and comparison cycles are complete. The
[experiment report](native-admission-cycles-2026-09-09.md) records fixed images,
measurements and retained failures. The final local-overlay-signature A/B/B/A
averaged **57,673 → 60,040 canonical logical transfers/s (+4.10%)**, with
**9.39% lower sampled validator CPU**. Both candidates beat both controls and
passed full proof/drain. Only `.env.desktop` enables that measured winner;
production defaults remain unchanged and the local images are not published.
Offered load did not establish maximum capacity.

Both admission candidates
remain default-off. Request sharing reduced manager requests by 94.7% but observed
59,219 canonical transfers/s versus its 59,834 control. Separately, one bounded
snapshot refresh reduced whole-run not-ready responses by 99.57%, while observing
57,904 canonical transfers/s versus its same-image 58,963 control. Both pairs
completed proof/drain and strict image checks. These are single-pair desktop
observations with no demonstrated TPS gain or independent offered-load margin;
they do not justify changing A's production defaults.

Local candidate flags are:

- `TON_OVERLAY_LOCAL_SIGNATURE_REUSE=0|1`: reuse exact, independently copied
  evidence from a successful local keyring signing callback. Incoming broadcast
  signatures still undergo ordinary verification. Global/physical default 0;
  tested desktop preset 1. See [the feature contract](overlay-local-signature-reuse.md).
- `TON_KEYRING_PREPARED_SIGNING=0|1`: reuse a successfully prepared immutable
  Ed25519 signing key with a fresh signing context for each request. Default 0;
  no clean positive throughput comparison supports promotion.
- `TON_NATIVE_ADMISSION_SHARD_SHARING=0|1`: bounded sharing of simultaneous
  exact-state admission shard-view requests, with independent caller deadlines.
- `TON_NATIVE_ADMISSION_SNAPSHOT_REFRESH=0|1`: at most one batch snapshot
  refresh within the original deadline. It rechecks mutable admission conditions
  and reuses only matching immutable signature evidence. Single-message admission
  keeps its existing strict snapshot rejection. Default 0; local correctness
  tests passed, but the completed desktop screen found no TPS gain.
- `TON_NATIVE_RECONCILIATION_PROFILE=0|1`: optional stage clocks for canonical
  reconciliation, default 0. Enable it before starting both diagnostic arms;
  always-on outcome counters distinguish unchanged accounts from necessary
  nonce/balance updates and expiry work. It changes measurement, not scheduling.

These variables take effect at validator startup. Set them in A's selected env
file and recreate/settle genesis before measurement, using the same prebuilt
image in both arms. Environment passthrough alone does not establish that an
image implements the feature. Check its revision and diagnostic counters; a
published `master` image does not acquire local changes automatically.

Updated profiler output includes shard requests/cache fills and optional
[reconciliation-local attribution](native-reconciliation-diagnostics.md),
implemented and measured in the completed refresh comparison.
Older recordings remain readable; current gauges and sequence numbers are
excluded from counter subtraction. Candidate `external_delivery_*` fields
separate published probes, producer state, first-epoch dispatch and subsequent
waiting; see [delivery semantics](native-collator-delivery-diagnostics.md).
Existing Session Stats revision `97c4f771` can ingest these additive fields.
Keep the eight-lane production database and prepare images once before an A/B
comparison; these desktop results do not constitute production promotion.

The admission profiling update adds exact-state configuration caching, stage and retry-cause counters,
and block-signature executor measurements. MyLocalTonDocker's
`benchmark/remote/admission-test-guide.md` contains the full A/B procedure and parameter table.
Keep the current eight-lane database intact and four lanes as the historical performance reference;
compare settings on the same topology first. Sixteen lanes remain deferred.

Prepare the new published TON image and derived wrappers once on A using
`bash start-native-genesis.sh --env-file .env`, then export with `--no-build-image` and import
that new bundle on B. Stop/drain the existing test before upgrading. Both arms of
each comparison must reuse this prebuilt image; do not pull/build between paired measurements.

A now accepts `TON_NATIVE_ADMISSION_CONFIG_CACHE=0|1` (default 1) and
`TON_NATIVE_VALIDATION_SIGNATURE_THREADS=1..64` (physical preset 8; fewer than 64 signed
parents remain serial). The latter tunes NTRN block-signature fanout independently of
`TON_NATIVE_EXECUTOR_THREADS=8` used by admission/state helpers. Existing eight-lane depth/split
values and candidate timeout/reserve remain unchanged. These are starting settings to test,
not an established optimum.

From MyLocalTonDocker on A, run the read-only sampler in a separate terminal before B:

```sh
sudo python3 benchmark/remote/profile-native-validator.py \
  --duration 1200 --interval 30 --dashboard-url http://127.0.0.1:18000 \
  --output "$HOME/native-profile-cache-on-01"
```

From the newly imported directory on B:

```sh
bash run-remote-load.sh --connections 10 --duration 600 --warmup 60 \
  --initial-cwnd 32768 --max-cwnd 65536 --submit-coalesce-ms 20
```

The standalone default is now one 10-connection arm, with 10 workers/32 signers and unchanged
40-CPU/48g ceilings and admission/backlog budgets. A saves counter deltas and raw per-thread,
CPU/cgroup and optional dashboard evidence. B saves `client-limits.json` even on successful runs,
plus measurement-phase window/RTT/retry/backlog gauges in `progress.jsonl`.
Use B's final timestamps to select A's matching samples; A's full sampling interval can include
readiness, warm-up and drain. Sampler Ctrl-C stops only sampling, never genesis.

Keep configuration caching on for the admission experiments. Recreate and settle A
identically, changing only the feature under test; repeat promising candidates
off/on/on/off with fixed images and B settings. Keep 20 ms coalescing, 10 connections
and existing windows as the control. Increase windows only when cap counters and
live gauges identify a binding credit limit. Thread-count changes remain separate,
profile-led experiments. Every result retains full proof, drain and safe source-reuse
gates. Full commands and artifact semantics are in the companion guide.


## Desktop reference measurements (2026-09-08)

The September 8 desktop measurements used a fresh four-lane reference,
10 connections, 600 measured seconds and 20 ms coalescing with fixed images.
[The completed desktop report](native-desktop-admission-benchmark-2026-09-08.md)
records the four strict-image-reuse tests, bootstrap workaround, canonical results
and the capacity-reporting correction. The highest observed result was 61,950
logical native transfers/s; the small differences between settings are not a
repeatable gain or a production-server capacity claim. Do not change an existing
production eight-lane database to the desktop topology.

The tracked `.env.desktop` now retains the September 9 winner, source `be235e03`,
using existing local images `mylocalton-genesis:admission-local-be235e03` and
`mylocalton-client:admission-local-be235e03`. Its runtime differs from the September
8 image; absolute rates across those builds are not a controlled comparison.
Docker Desktop must retain the tested **64 GiB** VM allocation on this 128 GB
workstation; its previous 99,840 MiB allocation caused host OOM. Four lanes and
all load/resource settings remain as measured. These are local CPU-specific
artifacts, not tags to pull on Server A.

On this desktop, start the selected configuration with strict image reuse:

```sh
cd /home/neodix/gitProjects/MyLocalTonDocker
docker compose --env-file .env.desktop \
  up -d --no-deps --no-build --pull never genesis
```

This preserves the existing database. Wait for genesis to become healthy and
settled before measuring. Do not use `start-native-genesis.sh` with these local
tags: it always pulls a registry base and rebuilds wrappers. The production
launcher remains appropriate for published Server A images, after their feature
revision has been verified.

On the healthy, settled four-lane desktop chain, run:

```sh
python3 benchmark/run-native-connections-sweep.py \
  --env-file .env.desktop --connections 10 --lane-depth 2 \
  --duration 600 --warmup 60 --drain 180 --coalesce-ms 20 \
  --initial-cwnd 32768 --max-cwnd 65536
```

A lower-latency comparison can use `--initial-cwnd 4096 --max-cwnd 8192`; it must
still pass complete drain/proof checks. Do not rebuild the images between arms.
