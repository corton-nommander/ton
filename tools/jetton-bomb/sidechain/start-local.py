#!/usr/bin/env python3
"""Start an isolated local v16 validator from an existing, immutable image."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import time


def docker(*args, **kwargs):
    return subprocess.check_output(["docker", *args], text=True, **kwargs)


def validate_isolated_container(state, image_id, expected_env, port):
    actual_env = dict(line.split("=", 1) for line in state["Config"]["Env"])
    if state["Image"] != image_id or any(actual_env.get(k) != v for k, v in expected_env.items()):
        raise ValueError("Existing container image/environment differs from the selected env file; select a fresh name")
    bindings = state["NetworkSettings"]["Ports"].get("40004/tcp")
    if bindings != [{"HostIp": "127.0.0.1", "HostPort": str(port)}]:
        raise ValueError(f"Container 40004/tcp must bind only to 127.0.0.1:{port}; got {bindings!r}")
    if state["HostConfig"]["NetworkMode"] == "host" or state["Mounts"]:
        raise ValueError("Isolated container must use its own network namespace and writable overlay without mounts")
    return actual_env


def parse_environment(text):
    return dict(line.split("=", 1) for line in text.splitlines() if line and not line.startswith("#"))


def wait_native_topology(config, checker, depth, timeout):
    if depth not in (1, 2, 3):
        raise ValueError("Native fixture requires a configured payment lane depth of 1, 2 or 3")
    deadline = time.monotonic() + timeout
    next_notice = 0
    observed = "not yet inspected"
    while time.monotonic() < deadline:
        try:
            check = subprocess.run([str(checker), "--ls-config", str(config), "--workchain", "-1", "--timeout", "20s"],
                                   capture_output=True, text=True, timeout=25)
            if check.returncode == 0:
                report = json.loads(check.stdout)
                count = report["basechain_shard_count"]
                depths = report["basechain_shard_depths"]
                observed = f"{count} leaves at depths {depths}"
                if report["native_payment_lanes"] and count == 1 << depth and depths == [depth]:
                    return report
            else:
                observed = check.stderr.strip()
        except (subprocess.TimeoutExpired, json.JSONDecodeError) as error:
            observed = str(error)
        if time.monotonic() >= next_notice:
            print(f"Waiting for {1 << depth} native leaves at depth {depth}: {observed}", flush=True)
            next_notice = time.monotonic() + 15
        time.sleep(2)
    raise TimeoutError(f"Native topology did not reach depth {depth} within {timeout}s; last observation: {observed}")


def export_native_wallets(container, output, count, actual_env):
    if actual_env.get("NATIVE_SPAM_RUN") != "0" or actual_env.get("SPAM_RUN") != "0":
        raise ValueError("Native fixture export requires both internal load producers to be disabled")
    marker_text = docker("exec", container, "cat", "/var/ton-work/db/native-spam/genesis.env")
    marker = parse_environment(marker_text)
    funded = int(marker["NATIVE_SPAM_GENESIS_SOURCES"])
    if count > funded or marker.get("NATIVE_SPAM_GENESIS_DESTINATIONS") != "1":
        raise ValueError(f"Requested {count} native source/destination pairs, but genesis funds only {funded}")
    if marker.get("NATIVE_SPAM_POST_GENESIS_TOPUPS") != "0" or marker.get("NATIVE_SPAM_TOPUP_MODE") != "genesis":
        raise ValueError("Native fixture export requires direct genesis funding without post-genesis topups")
    if marker.get("NATIVE_TRANSFER_RUNS_ENABLED") != "1" or marker.get("NATIVE_PAYMENT_LANES_ENABLED") != "1":
        raise ValueError("Native fixture requires source-signed runs and fixed payment lanes")
    wallet_dir = marker["NATIVE_SPAM_WALLET_DIR"]
    if not wallet_dir.startswith("/"):
        raise ValueError("Genesis wallet directory must be absolute")
    wallets = output / "native-wallets"
    wallets.mkdir(mode=0o700, exist_ok=True)
    wallets.chmod(0o700)
    public_hashes = {}
    # Keep argv and captured archive size bounded, including for larger fixtures.
    # Never export destination keys or unrelated wallet/database files.
    for first in range(0, count, 256):
        names = [name for i in range(first, min(count, first + 256))
                 for name in (f"source-{i}.pk", f"source-{i}.pub", f"dest-{i}.pub")]
        result = subprocess.run(["docker", "exec", container, "tar", "-C", wallet_dir, "-cf", "-", *names],
                                capture_output=True, check=True, timeout=30)
        seen = set()
        with tarfile.open(fileobj=io.BytesIO(result.stdout), mode="r:") as archive:
            for member in archive:
                if member.name not in names or member.name in seen or not member.isfile() or member.size != 32:
                    raise ValueError(f"Unexpected native fixture archive member: {member.name}")
                data = archive.extractfile(member).read()
                with tempfile.NamedTemporaryFile(dir=wallets, delete=False) as temporary:
                    temporary.write(data)
                    temporary_path = Path(temporary.name)
                temporary_path.chmod(0o600)
                temporary_path.replace(wallets / member.name)
                seen.add(member.name)
                if member.name.endswith(".pub"):
                    public_hashes[member.name] = hashlib.sha256(data).hexdigest()
        if seen != set(names):
            raise ValueError("Native fixture archive is incomplete")
    manifest = docker("exec", container, "cat", f"{wallet_dir}/native-payment-lanes.manifest")
    (output / "native-genesis.env").write_text(marker_text)
    (wallets / "native-payment-lanes.manifest").write_text(manifest)
    (output / "native-fixture.json").write_text(json.dumps({"sources": count, "first_index": 0,
        "genesis_funded_sources": funded, "payment_lane_depth": int(marker["NATIVE_PAYMENT_LANE_DEPTH"]),
        "source_wallet_directory": wallet_dir, "exported_files": ["source-N.pk", "source-N.pub", "dest-N.pub"],
        "public_file_sha256": public_hashes,
        "manifest_sha256": hashlib.sha256(manifest.encode()).hexdigest()}, indent=2) + "\n")


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="sidechain-jetton:account-none-20261005")
    parser.add_argument("--name", default="sidechain-jetton-bench")
    parser.add_argument("--port", type=int, default=41004)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--env-file", type=Path, default=here / "local.env", help="Fresh genesis environment, also checked on reuse")
    parser.add_argument("--native-sources", type=int, default=0, help="Export this many funded native fixture pairs (0 disables export)")
    parser.add_argument("--wait-topology-timeout", type=int, default=600, help="Seconds to wait for the full configured native lane topology")
    parser.add_argument("--reuse", action="store_true", help="Export materials from this existing isolated container")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if not 0 <= args.native_sources <= 65536:
        parser.error("--native-sources must be between 0 and 65536")
    if not 1 <= args.wait_topology_timeout <= 1800:
        parser.error("--wait-topology-timeout must be between 1 and 1800 seconds")
    checker = here.parent / "go-spam/build/sidechaincheck"
    if args.native_sources and not checker.is_file():
        parser.error("Native fixture startup requires make -C tools/jetton-bomb sidechaincheck")
    env_file = args.env_file.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    image = json.loads(docker("image", "inspect", args.image))[0]
    expected_env = parse_environment(env_file.read_text())
    if not args.reuse:
        print(docker("run", "-d", "--name", args.name, "--init", "--cpus", "16", "--memory", "12g", "--memory-swap", "12g",
                     "-p", f"127.0.0.1:{args.port}:40004", "--env-file", str(env_file), image["Id"]).strip(), flush=True)
    else:
        existing = json.loads(docker("inspect", args.name))[0]
        try:
            validate_isolated_container(existing, image["Id"], expected_env, args.port)
        except ValueError as error:
            parser.error(str(error))
    deadline = time.monotonic() + 180
    next_notice = 0
    while time.monotonic() < deadline:
        state = json.loads(docker("inspect", args.name))[0]
        if not state["State"]["Running"]:
            raise RuntimeError(f"Validator stopped: {docker('logs', '--tail', '30', args.name)}")
        actual_env = validate_isolated_container(state, image["Id"], expected_env, args.port)
        result = subprocess.run(["docker", "exec", args.name, "/usr/local/bin/lite-client", "-a", "127.0.0.1:40004",
                                 "-p", "/var/ton-work/db/liteserver.pub", "-v", "0", "-t", "3", "-c", "last"],
                                capture_output=True, text=True, errors="replace", timeout=10)
        block_match = re.search(r"latest masterchain block known to server is \(-1,8000000000000000,(\d+)\)",
                                result.stdout + result.stderr)
        if result.returncode == 0 and block_match and int(block_match[1]) > 0:
            break
        if time.monotonic() >= next_notice:
            print("Waiting for isolated validator to produce blocks...", flush=True)
            next_notice = time.monotonic() + 15
        time.sleep(2)
    else:
        raise TimeoutError("Validator did not become ready within 180 seconds")
    for source, target in [("/var/ton-work/db/localhost.global.config.json", "local-config.json"),
                           ("/usr/share/ton/smartcont/main-wallet.addr", "owner.addr"),
                           ("/usr/share/ton/smartcont/main-wallet.pk", "owner.pk")]:
        docker("cp", f"{args.name}:{source}", str(output / target))
    (output / "owner.pk").chmod(0o600)
    config_path = output / "local-config.json"
    config = json.loads(config_path.read_text())
    config["liteservers"][0]["port"] = args.port
    config_path.write_text(json.dumps(config, indent=2) + "\n")
    address = (output / "owner.addr").read_bytes()
    if len(address) != 36:
        raise ValueError("Unexpected Fift wallet address length")
    raw = f"{int.from_bytes(address[32:], 'big', signed=True)}:{address[:32].hex()}"
    (output / "owner-address").write_text(raw + "\n")
    if args.native_sources:
        topology = wait_native_topology(config_path, checker, int(actual_env["NATIVE_PAYMENT_LANE_DEPTH"]), args.wait_topology_timeout)
        (output / "native-topology.json").write_text(json.dumps(topology, indent=2) + "\n")
        export_native_wallets(args.name, output, args.native_sources, actual_env)
    (output / "container.json").write_text(json.dumps({"name": args.name, "image_id": image["Id"],
        "source_labels": image["Config"]["Labels"], "environment": actual_env,
        "resources": {k: state["HostConfig"][k] for k in ["NanoCpus", "Memory", "MemorySwap", "CpusetCpus"]},
        "host_config": state["HostConfig"], "mounts": state["Mounts"],
        "port_bindings": state["NetworkSettings"]["Ports"],
        "ready_masterchain_seqno": int(block_match[1]), "environment_file": str(env_file),
        "native_exported_sources": args.native_sources,
        "reused_container": args.reuse,
        "storage": ("existing isolated container writable overlay" if args.reuse else "fresh isolated container writable overlay")},
        indent=2) + "\n")
    print(f"Ready: config={config_path} owner={raw} wallet=v3 subwallet=42", flush=True)
    print(f"Stop after use: docker stop {args.name}", flush=True)


if __name__ == "__main__":
    main()
