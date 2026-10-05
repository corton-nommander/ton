#!/usr/bin/env python3
"""Wrap the locally built validator in an existing genesis image for local tests.

The local Release build uses the host glibc. Bundle its loader and shared
libraries explicitly so a Ubuntu 22 genesis image can run that exact binary.
This is a local benchmark helper; the repository Dockerfile remains the normal
way to build a complete deployment image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    repo = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validator", type=Path, required=True)
    parser.add_argument("--base", default="mylocalton-genesis:histcurrent-20260914-current")
    parser.add_argument("--tag", default="sidechain-jetton:account-none-20261005")
    args = parser.parse_args()
    binary = args.validator.resolve(strict=True)
    base = json.loads(subprocess.check_output(["docker", "image", "inspect", args.base], text=True))[0]
    dependencies = subprocess.check_output(["ldd", str(binary)], text=True)
    if "not found" in dependencies:
        raise RuntimeError("Validator has missing shared libraries:\n" + dependencies)
    with tempfile.TemporaryDirectory(prefix="jetton-sidechain-image-") as temp:
        context = Path(temp)
        runtime = context / "runtime"
        runtime.mkdir()
        hashes = {}
        for line in dependencies.splitlines():
            match = re.search(r"(?:=> )?(/[^ ]+) \(", line)
            if match:
                source = Path(match.group(1))
                target = runtime / source.name
                shutil.copyfile(source, target)
                hashes[target.name] = hashlib.sha256(target.read_bytes()).hexdigest()
        if not (runtime / "ld-linux-x86-64.so.2").exists():
            raise RuntimeError("This local helper requires a Linux x86_64 dynamically linked validator")
        shutil.copyfile(binary, context / "validator-engine")
        (context / "validator-wrapper").write_text('#!/bin/sh\nexec /opt/host-runtime/ld-linux-x86-64.so.2 --library-path /opt/host-runtime /usr/local/libexec/validator-engine "$@"\n')
        # BuildKit requires a name or registry digest in FROM, not a local image ID.
        pinned_tag = "sidechain-jetton-base:" + base["Id"].split(":")[1][:16]
        subprocess.run(["docker", "tag", base["Id"], pinned_tag], check=True)
        provenance = {"base_image_id": base["Id"], "validator_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                      "validator_build_info": subprocess.check_output([str(binary), "--version"], text=True).strip(),
                      "runtime_libraries_sha256": hashes,
                      "source_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True, cwd=repo).strip(),
                      "source_diff": subprocess.check_output(["git", "diff", "--", "crypto/block/block-parse.cpp"], text=True, cwd=repo)}
        (context / "validator-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
        (context / "Dockerfile").write_text(f"FROM {pinned_tag}\nCOPY --chmod=755 runtime/ /opt/host-runtime/\n"
            "COPY --chmod=755 validator-engine /usr/local/libexec/validator-engine\n"
            "COPY --chmod=755 validator-wrapper /usr/local/bin/validator-engine\n"
            "COPY validator-provenance.json /usr/local/share/validator-provenance.json\n")
        subprocess.run(["docker", "build", "--pull=false", "-t", args.tag, str(context)], check=True)
    print(json.dumps(provenance, indent=2))


if __name__ == "__main__":
    main()
