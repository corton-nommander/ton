# One public validator and a remote native TPS generator

This guide describes the code inspected on 2026-09-07: sidechain result commit `4b2034a8` and MyLocalTonDocker `44ffe58`. No remote run or public exposure was performed while preparing it.

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

Set the public **IPv4 address**, without a URL prefix. The example address below is a documentation placeholder.

```sh
export SERVER_A_IP=203.0.113.10
umask 077
mkdir -m 700 "$HOME/native-client-export"
docker cp genesis:/usr/share/data/global.config.json \
  "$HOME/native-client-export/internal.global.config.json"

python3 - "$SERVER_A_IP" <<'PYCONFIG'
import ipaddress, json, os, pathlib, sys
root = pathlib.Path(os.environ['HOME']) / 'native-client-export'
config = json.loads((root / 'internal.global.config.json').read_text())
assert len(config['liteservers']) == 1, 'Expected exactly one liteserver'
ip = int(ipaddress.IPv4Address(sys.argv[1]))
config['liteservers'][0]['ip'] = ip if ip < 2**31 else ip - 2**32
config['liteservers'][0]['port'] = 40004
(root / 'external.global.config.json').write_text(json.dumps(config, indent=2) + '\n')
PYCONFIG
```

This retains the liteserver public key and every chain hash, including the zero-state signing domain. The `external.global.config.json` is public. Keep the wallet archive private: it contains funded benchmark **source** keys. The following selects only the source signing keys, public source/destination material and lane manifest, without copying the validator database or destination signing keys:

```sh
docker exec -i genesis python3 - <<'PYWALLETS' > "$HOME/native-client-export/test-wallets.tar.gz"
import pathlib, sys, tarfile
root = pathlib.Path('/var/ton-work/db/native-spam/wallets')
header = (root / 'native-payment-lanes.manifest').read_text().splitlines()[0].split()
assert header[:3] == ['NATIVE_PAYMENT_LANES_MANIFEST_V1', '2', '4']
assert int(header[3]) >= 24576
names = ['native-payment-lanes.manifest']
for i in range(24576):
    names.extend([f'source-{i}.pk', f'source-{i}.pub', f'source-{i}.addr',
                  f'dest-{i}.pub', f'dest-{i}.addr'])
assert all((root / name).is_file() for name in names)
with tarfile.open(fileobj=sys.stdout.buffer, mode='w|gz') as archive:
    for name in names:
        archive.add(root / name, arcname=name, recursive=False)
PYWALLETS
```

Stop any other generator using those sources before running B. Auto-nonce discovers canonical state; it does not coordinate competing writers. Keep unrelated native traffic off during the measurement because chain TPS includes all native transfers in observed blocks. Synchronize A and B clocks through NTP: B's measurement window is compared with A's block timestamps.

## Transfer the prebuilt generator

Cloning MyLocalTonDocker on B is optional for `docker run`; B needs Docker, Bash, Python 3 and the image/config/test keys. Cloning source does not copy Docker images or funded accounts. If cloning, ensure the fork includes harness `44ffe58`; these local commits were not automatically pushed.

On the machine that already has the tested generator image (the desktop, or A if previously transferred), export it:

```sh
docker image save -o native-generator-image.tar \
  mylocalton-native-load-generator:cycle-clients-ed666c9a-h2
```

Copy this archive to B using SCP and load it **before** measuring. Use compatible CPU architecture/instruction support on B; a CPU-specific image may need a separately prebuilt compatible generator image.

```sh
# On B; replace user, A and the image archive source as appropriate.
umask 077
mkdir -p client-data/wallets remote-results
scp user@SERVER_A:~/native-client-export/external.global.config.json client-data/global.config.json
scp user@SERVER_A:~/native-client-export/test-wallets.tar.gz ./test-wallets.tar.gz
tar -xzf test-wallets.tar.gz -C client-data/wallets
chmod -R go-rwx client-data
# Copy native-generator-image.tar here from the image-owning machine first.
docker image load -i native-generator-image.tar
```

Copy the adjacent [native-remote-load.env](benchmarks/native-remote-load.env) to B's working directory as `remote-load.env`. It is a complete public preset extracted from the successful 100-connection run, with paths changed for a read-only `/client` bind mount. It preserves the retry, expiry and canonical-follower settings as well as the visible windows/batching settings. Do not use the general Compose `.env` as a Docker `--env-file`: it contains different defaults and Compose-specific syntax.

For the exact saved image, use its immutable ID:

```sh
export REMOTE_LOAD_IMAGE=sha256:322c9b5cc6e74d884563d97008b53d76979a584eec05dfa875f0a6bdc2313871
docker image inspect "$REMOTE_LOAD_IMAGE" --format '{{.Id}}'
docker run --rm --pull never --network host \
  --mount type=bind,src="$PWD/client-data",dst=/client,readonly \
  --entrypoint /usr/local/bin/lite-client "$REMOTE_LOAD_IMAGE" \
  -C /client/global.config.json -t 10 -c last
```

`--network host` here assumes Linux server B. B needs outbound TCP to A:40004, not an inbound published client port. The config supplies the liteserver key; no separate `.pub` argument is necessary. The generator uses persistent ADNL/TCP and the batch RPC, with one additional canonical-follower connection beyond the requested submission count.

## Run 10, 50 and 100 connections on B

The following is a **standalone shell example**, not a new remote mode in the repository's local Python sweep. Save it on B as `run-remote-load.sh`, next to `remote-load.env` and `client-data`:

```bash
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" != --connections || $# -lt 2 ]]; then
  echo 'Usage: bash run-remote-load.sh --connections 10 50 100' >&2
  exit 2
fi
shift
for connections in "$@"; do
  [[ "$connections" =~ ^[1-9][0-9]{0,2}$ ]] &&
    (( connections >= 6 && connections <= 256 )) || {
      echo 'This six-worker preset requires 6..256 connections' >&2
      exit 2
    }
done
image=${REMOTE_LOAD_IMAGE:?Set REMOTE_LOAD_IMAGE to the prebuilt immutable image ID}
docker image inspect "$image" >/dev/null
run_id=$(date -u +%Y%m%dT%H%M%SZ)
output="$PWD/remote-results/$run_id"
mkdir -p "$PWD/remote-results"
mkdir "$output"
cp remote-load.env "$output/remote-load.env"
cp client-data/global.config.json "$output/global.config.json"
printf '%s\n' "$image" > "$output/image-id.txt"
active_container=
trap 'if [[ -n "$active_container" ]]; then docker stop -t 30 "$active_container" >/dev/null || true; fi' EXIT
for connections in "$@"; do
  active_container="native-remote-${run_id}-${connections}"
  echo "Running $connections connections; metrics: $output/$connections.jsonl"
  status=0
  docker run --pull never --name "$active_container" \
    --network host --cpus 4 --memory 8g \
    --mount type=bind,src="$PWD/client-data",dst=/client,readonly \
    --env-file "$PWD/remote-load.env" \
    -e NATIVE_LOAD_CONNECTIONS="$connections" \
    "$image" > "$output/$connections.jsonl" 2> "$output/$connections.stderr.log" || status=$?
  docker inspect "$active_container" > "$output/$connections.container.json"
  (( status == 0 )) || exit "$status"
  active_container=
  python3 - "$output/$connections.jsonl" <<'PYRESULT'
import json, pathlib, sys
rows = [json.loads(line) for line in pathlib.Path(sys.argv[1]).read_text().splitlines()
        if line.startswith('{')]
finals = [row for row in rows if row.get('final') is True]
assert len(finals) == 1, 'Missing or ambiguous final record; stop the sequence'
r = finals[0]
keys = ['configured_connections', 'steady_offered_avg_tps', 'steady_mempool_accept_avg_tps',
        'canonical_chain_measure_avg_tps', 'benchmark_result_valid', 'chain_capacity_valid',
        'chain_capacity_invalid_reasons', 'canonical_backlog']
print(json.dumps({key: r.get(key) for key in keys}, indent=2))
assert r.get('benchmark_result_valid') is True and r.get('canonical_backlog') == 0, \
    'Incomplete or incorrect run; inspect evidence before reusing these source keys'
PYRESULT
done
```

Run the requested sequence or one count:

```sh
bash run-remote-load.sh --connections 10 50 100
# For a later independent run:
# bash run-remote-load.sh --connections 50
```

Each arm uses unpaced bounded load, 60 seconds of warm-up, 180 seconds measured load, and up to 180 seconds of drain, plus initial account/lane readiness work. The environment fixes six workers/signers, 24,576 sources, 16 logical transfers per signed run, a 64-parent batch cap, 20 ms coalescing, and global initial/max admission windows of 32,768/65,536 logical transfers. Containers and raw output are retained. Choose CPU/memory limits appropriate to B before declaring an experiment; changing them changes the comparison. No generator starts on A.

For current progress, read the JSONL file in another terminal. If interrupted or a drain fails, inspect/stop the named generator and reconcile pending work before starting a new arm with the same keys. A clean generator exit and a true final `benchmark_result_valid` are required, not merely a high dashboard peak.

Use `canonical_chain_measure_avg_tps` for block-time canonical throughput; `steady_mempool_accept_avg_tps` measures admission. The shell stops on incomplete/incorrect runs but retains capacity-rejected observations, just as the local sweep distinguishes observations from capacity results. Inspect signed-run density and lane validity too. This shell does not independently collect A's validator process identity, resources, logs or mempool-cleanup evidence. Preserve A-side evidence separately; do not label its output as having passed the full local harness's strict capacity acceptance. Keep A unchanged during the entire sequence and ensure offered load exceeds canonical TPS before making a capacity claim.

## Bandwidth for the same approximately 60k TPS workload

60,000 TPS means **60,000 logical transfers/s**, packed into **3,750 new signed parents/s**. The measured generator repeats an identical destination/amount/fee 16 times within each parent; BOC cell deduplication makes that parent approximately **260 bytes**. Consequently, the fresh BOC payload alone is **7.8 Mbit/s B → A**.

Using the completed 10/50/100 sweep's whole-run retry and query proportions, estimated request traffic at 60k new logical TPS is **11.62 / 14.42 / 14.52 Mbit/s B → A** including TL/ADNL framing, before TCP/IP and retransmissions. At 100 connections there were approximately 1.784 physical attempts per fresh parent.

The canonical follower downloads complete blocks in the other direction. Candidate block payloads in these runs suggest roughly **33–35 Mbit/s A → B** at 60k, before proof/masterchain/account traffic and retries. This is a planning proxy, not measured canonical socket traffic. Actual 100-client Docker samples averaged 10.76 Mbit/s TX and 26.89 Mbit/s RX over 363 seconds including setup/warm-up/drain; they do not establish the steady-window WAN minimum.

An exact minimum has not been measured. For planning, avoid links below **100 Mbit/s full duplex** and prefer a dedicated **1 Gbit/s full-duplex link** for comfortable headroom and bursts. These are engineering allowances, not proof that a particular link will sustain 60k. On A, the larger requirement is upload toward B; on B it is download from A. Latency, packet loss and application credit windows also affect achieved TPS.

This estimate is workload-specific: 16 distinct outputs instead of repeated identical outputs produce approximately **1,018-byte parents**, raising fresh BOC request payload to about **30.5 Mbit/s before retries**. Sixty thousand independent scalar messages/s is also a different test from sixty thousand batched logical transfers/s.

Source references: `lite-client/native-load-generator.cpp:3725` (outputs), `crypto/vm/boc.cpp:223` (deduplication), `adnl/adnl-ext-connection.cpp:30` (framing), and `lite-client/native-load-generator.cpp:2603` (block downloads). The [saved desktop report](native-client-connections-2026-09-06.md) links original counters and classifications. A repeated remote benchmark with physical interface byte counters is needed to replace these estimates with a measured network requirement.
