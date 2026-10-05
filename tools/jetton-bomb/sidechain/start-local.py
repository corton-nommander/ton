#!/usr/bin/env python3
"""Start an isolated local v16 validator from an existing, immutable image."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time


def docker(*args, **kwargs):
    return subprocess.check_output(["docker", *args], text=True, **kwargs)


def validate_isolated_container(state, image_id, expected_env, port):
    actual_env = dict(line.split("=", 1) for line in state["Config"]["Env"])
    if state["Image"] != image_id or any(actual_env.get(k) != v for k, v in expected_env.items()):
        raise ValueError("Existing container image/environment differs from local.env; select a fresh name")
    bindings = state["NetworkSettings"]["Ports"].get("40004/tcp")
    if bindings != [{"HostIp": "127.0.0.1", "HostPort": str(port)}]:
        raise ValueError(f"Container 40004/tcp must bind only to 127.0.0.1:{port}; got {bindings!r}")
    if state["HostConfig"]["NetworkMode"] == "host" or state["Mounts"]:
        raise ValueError("Isolated container must use its own network namespace and writable overlay without mounts")
    return actual_env


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="sidechain-jetton:account-none-20261005")
    parser.add_argument("--name", default="sidechain-jetton-bench")
    parser.add_argument("--port", type=int, default=41004)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reuse", action="store_true", help="Export materials from this existing isolated container")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    image = json.loads(docker("image", "inspect", args.image))[0]
    expected_env = dict(line.split("=", 1) for line in (here / "local.env").read_text().splitlines() if line and not line.startswith("#"))
    if not args.reuse:
        print(docker("run", "-d", "--name", args.name, "--init", "--cpus", "16", "--memory", "12g", "--memory-swap", "12g",
                     "-p", f"127.0.0.1:{args.port}:40004", "--env-file", str(here / "local.env"), image["Id"]).strip(), flush=True)
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
    (output / "container.json").write_text(json.dumps({"name": args.name, "image_id": image["Id"],
        "source_labels": image["Config"]["Labels"], "environment": actual_env,
        "resources": {k: state["HostConfig"][k] for k in ["NanoCpus", "Memory", "MemorySwap", "CpusetCpus"]},
        "host_config": state["HostConfig"], "mounts": state["Mounts"],
        "port_bindings": state["NetworkSettings"]["Ports"],
        "ready_masterchain_seqno": int(block_match[1]),
        "reused_container": args.reuse,
        "storage": ("existing isolated container writable overlay" if args.reuse else "fresh isolated container writable overlay")},
        indent=2) + "\n")
    print(f"Ready: config={config_path} owner={raw} wallet=v3 subwallet=42", flush=True)
    print(f"Stop after use: docker stop {args.name}", flush=True)


if __name__ == "__main__":
    main()
