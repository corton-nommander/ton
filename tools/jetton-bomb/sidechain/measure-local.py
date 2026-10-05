#!/usr/bin/env python3
"""Run internalspam with local materials and save validator resource samples."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import signal
import subprocess
import time


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--materials", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--container", default="sidechain-jetton-bench")
    parser.add_argument("--contracts", type=int, default=4)
    parser.add_argument("--nonce", type=int, required=True, help="Use a fresh range for each run")
    parser.add_argument("--duration", default="120s")
    parser.add_argument("--warmup", default="10s")
    parser.add_argument("--target-tps", type=int, default=3000)
    args = parser.parse_args()
    if not 1 <= args.contracts <= 256:
        parser.error("--contracts must be between 1 and 256")

    def interrupt(signum, frame):
        raise KeyboardInterrupt(f"signal {signum}")

    signal.signal(signal.SIGTERM, interrupt)
    materials = args.materials.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [str(here.parent / "go-spam/build/internalspam"), "run", "--ls-config", str(materials / "local-config.json"),
               "--code", str(here / "assets/StaticTps.compiled.json"), "--owner", (materials / "owner-address").read_text().strip(),
               "--owner-key", str(materials / "owner.pk"), "--wallet-version", "v3", "--subwallet-id", "42", "--workchain", "-1",
               "--contracts", str(args.contracts), "--nonce", str(args.nonce), "--target-tps", str(args.target_tps),
               "--duration", args.duration, "--warmup", args.warmup, "--funding", "1000000", "--output", str(output)]
    started = time.monotonic()
    samples = []
    next_notice = 0
    with output.with_suffix(".log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None:
                stats = subprocess.run(["docker", "stats", "--no-stream", "--format", "{{json .}}", args.container],
                                       capture_output=True, text=True, timeout=15)
                sample = {"at": datetime.now(timezone.utc).isoformat(), "elapsed_seconds": time.monotonic() - started}
                if stats.returncode == 0:
                    sample["docker_stats"] = json.loads(stats.stdout)
                else:
                    sample["error"] = stats.stderr.strip()
                samples.append(sample)
                elapsed = time.monotonic() - started
                if elapsed >= next_notice:
                    print(f"internalspam contracts={args.contracts}, elapsed={elapsed:.0f}s", flush=True)
                    next_notice = elapsed + 30
                for _ in range(10):
                    if process.poll() is not None:
                        break
                    time.sleep(1)
        except BaseException:
            process.send_signal(2)
            process.wait(timeout=90 + 60 * ((args.contracts + 3) // 4))
            raise
        finally:
            output.with_suffix(".telemetry.json").write_text(json.dumps({"command": command, "samples": samples}, indent=2) + "\n")
    if process.returncode:
        raise SystemExit(f"internalspam failed ({process.returncode}); inspect {output} and {output.with_suffix('.log')}")
    result = json.loads(output.read_text())
    print(json.dumps({k: result[k] for k in ["valid", "canonical_window_seconds", "wall_window_seconds", "successful_work_tps",
                                           "successful_work_wall_tps", "internal_transaction_tps", "internal_transaction_wall_tps",
                                           "receipts", "drained", "audit_complete"]}, indent=2))


if __name__ == "__main__":
    main()
