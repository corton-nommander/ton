#!/usr/bin/env python3
"""Run jetton-bomb native payments externally and save local resource evidence.

The Go runner owns canonical payment/parent accounting, account anchors and
drain verification. This helper starts no validator-internal load producer.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import time


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def container_info(name, materials):
    recorded = json.loads((materials / "container.json").read_text())
    state = json.loads(subprocess.check_output(["docker", "inspect", name], text=True, timeout=15))[0]
    actual_env = dict(line.split("=", 1) for line in state["Config"]["Env"])
    if not state["State"]["Running"] or state["State"].get("Paused"):
        raise ValueError("The selected isolated validator must be running")
    if state["Image"] != recorded["image_id"] or state["Mounts"] or state["HostConfig"]["NetworkMode"] == "host":
        raise ValueError("Validator image/storage/network differs from the exported isolated fixture")
    if actual_env.get("NATIVE_SPAM_RUN") != "0" or actual_env.get("SPAM_RUN") != "0":
        raise ValueError("Validator-internal load producers must be disabled")
    if any(actual_env.get(k) != v for k, v in recorded["environment"].items()):
        raise ValueError("Validator environment differs from exported materials")
    config = json.loads((materials / "local-config.json").read_text())
    if len(config.get("liteservers", [])) != 1 or config["liteservers"][0]["ip"] != 2130706433:
        raise ValueError("Local native benchmark requires one loopback liteserver")
    port = str(config["liteservers"][0]["port"])
    expected_binding = [{"HostIp": "127.0.0.1", "HostPort": port}]
    if state["NetworkSettings"]["Ports"].get("40004/tcp") != expected_binding:
        raise ValueError("Liteserver endpoint differs from the isolated validator port binding")
    return {"name": name, "image_id": state["Image"], "environment": actual_env,
            "resources": {k: state["HostConfig"][k] for k in ("NanoCpus", "Memory", "MemorySwap", "CpusetCpus")},
            "mounts": state["Mounts"], "port_bindings": state["NetworkSettings"]["Ports"]}


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--materials", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--container", default="sidechain-native-bench")
    parser.add_argument("--sources", type=int, default=1024)
    parser.add_argument("--duration", default="120s")
    parser.add_argument("--warmup", default="10s")
    parser.add_argument("--outputs", type=int, default=16, help="Logical payments per signed NTRN parent")
    parser.add_argument("--lane-depth", type=int, help="Expected fixed lane depth; defaults to exported fixture depth")
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--batch-size", type=int, default=64, help="Signed parents per submission batch")
    parser.add_argument("--window", type=int, default=64)
    parser.add_argument("--amount", type=int, default=1, help="Nanotokens per logical payment")
    parser.add_argument("--fee", type=int, default=0, help="Nanotokens per logical payment")
    parser.add_argument("--rate", type=float, default=0, help="Logical offered payments/s; 0 leaves pacing to the runner")
    parser.add_argument("--drain-timeout", default="120s")
    parser.add_argument("--cleanup-wait-seconds", type=float, default=180,
                        help="Maximum helper wait after requesting child cancellation")
    args = parser.parse_args()
    if not 1 <= args.sources <= 65536 or not 1 <= args.outputs <= 16:
        parser.error("--sources must be 1..65536 and --outputs must be 1..16")
    if not 1 <= args.workers <= 1024 or not 1 <= args.batch_size <= 1024 or args.window < 1:
        parser.error("invalid worker/batch/window bounds")
    if args.amount < 1 or args.fee < 0 or args.amount + args.fee > 2**64 - 1:
        parser.error("amount must be positive and amount + fee must fit uint64")
    if not math.isfinite(args.rate) or args.rate < 0 or not math.isfinite(args.cleanup_wait_seconds) or args.cleanup_wait_seconds <= 0:
        parser.error("rate must be finite and nonnegative; cleanup wait must be finite and positive")

    def interrupt(signum, frame):
        raise KeyboardInterrupt(f"signal {signum}")

    signal.signal(signal.SIGTERM, interrupt)
    materials = args.materials.resolve(strict=True)
    output = args.output.resolve()
    if output.exists():
        parser.error("--output already exists; use a new filename for each measurement")
    output.parent.mkdir(parents=True, exist_ok=True)
    fixture = json.loads((materials / "native-fixture.json").read_text())
    if args.sources > fixture["sources"] or fixture["first_index"] != 0:
        parser.error("requested source range exceeds the exported native fixture")
    depth = fixture["payment_lane_depth"] if args.lane_depth is None else args.lane_depth
    if depth not in (1, 2, 3) or depth != fixture["payment_lane_depth"]:
        parser.error("--lane-depth must match the exported fixture depth (1, 2 or 3)")
    binary = here.parent / "go-spam/build/nativespam"
    config = materials / "local-config.json"
    wallet_dir = materials / "native-wallets"
    runtime = container_info(args.container, materials)
    command = [str(binary), "run", "--config", str(config), "--wallet-dir", str(wallet_dir),
               "--sources", str(args.sources), "--duration", args.duration, "--warmup", args.warmup,
               "--outputs", str(args.outputs), "--workers", str(args.workers), "--batch-size", str(args.batch_size),
               "--lane-depth", str(depth),
               "--window", str(args.window), "--amount", str(args.amount), "--fee", str(args.fee),
               "--rate", str(args.rate), "--drain-timeout", args.drain_timeout, "--output", str(output)]
    provenance = {"command": command, "client_binary_sha256": sha256(binary), "config_sha256": sha256(config),
                  "fixture": fixture, "validator": runtime,
                  "measurement_scope": "Go runner canonical native logical payments and signed NTRN parents; RPC submission counts are not committed TPS",
                  "telemetry_scope": "whole command; select samples between result start/end capture times for measurement-only summaries"}
    provenance_path = output.with_suffix(".provenance.json")
    provenance_path.write_text(json.dumps(provenance, indent=2) + "\n")
    started = time.monotonic()
    samples = []
    next_notice = 0
    with output.with_suffix(".log").open("w") as log:
        process = None
        try:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            while process.poll() is None:
                sample = {"at": datetime.now(timezone.utc).isoformat(), "elapsed_seconds": time.monotonic() - started}
                try:
                    stats = subprocess.run(["docker", "stats", "--no-stream", "--format", "{{json .}}", args.container],
                                           capture_output=True, text=True, timeout=15)
                    if stats.returncode == 0:
                        sample["validator_docker_stats"] = json.loads(stats.stdout)
                    else:
                        sample["validator_stats_error"] = stats.stderr.strip()
                except (subprocess.TimeoutExpired, json.JSONDecodeError) as error:
                    sample["validator_stats_error"] = str(error)
                sample["at"] = datetime.now(timezone.utc).isoformat()
                sample["elapsed_seconds"] = time.monotonic() - started
                # Linux proc values measure the external Go process separately
                # from validator CPU/memory; save cumulative ticks for analysis.
                try:
                    fields = (Path(f"/proc/{process.pid}/stat").read_text().rsplit(")", 1)[1]).split()
                    sample["client_process"] = {"pid": process.pid, "user_ticks": int(fields[11]),
                                                "system_ticks": int(fields[12]), "rss_pages": int(fields[21])}
                except (FileNotFoundError, ProcessLookupError):
                    pass
                samples.append(sample)
                elapsed = time.monotonic() - started
                if elapsed >= next_notice:
                    print(f"nativespam sources={args.sources}, outputs={args.outputs}, elapsed={elapsed:.0f}s", flush=True)
                    next_notice = elapsed + 30
                for _ in range(10):
                    if process.poll() is not None:
                        break
                    time.sleep(1)
        except BaseException:
            if process is not None and process.poll() is None:
                process.send_signal(signal.SIGINT)
                process.wait(timeout=args.cleanup_wait_seconds)
            raise
        finally:
            output.with_suffix(".telemetry.json").write_text(json.dumps({"command": command,
                "client_clock_ticks_per_second": os.sysconf("SC_CLK_TCK"),
                "client_page_size_bytes": os.sysconf("SC_PAGE_SIZE"), "samples": samples}, indent=2) + "\n")
    if process.returncode:
        raise SystemExit(f"nativespam failed ({process.returncode}); inspect {output} and {output.with_suffix('.log')}")
    result = json.loads(output.read_text())
    scalar_summary = {k: v for k, v in result.items() if isinstance(v, (str, int, float, bool)) or v is None}
    print(json.dumps(scalar_summary, indent=2))


if __name__ == "__main__":
    main()
