#!/usr/bin/env python3
"""Run offline regressions against the real native-load worker implementation.

First build native-load-generator, then run:
  python3 lite-client/tests/native-load-worker-regression.py --build-dir build

The fixture exposes private worker members only in a temporary source copy and
renames its main function. It uses the existing CMake/Ninja compiler and linker
arguments, so no production test hooks or network endpoint are required. Output
and source fingerprints are retained in the printed artifact directory. Expected
quarantine/conflict diagnostics, and the offline coordinator sink's deliberate
configuration failure, are kept in result.json rather than mixed with PASS lines.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shlex
import subprocess
import sys
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ninja_commands(build_dir, target):
    result = subprocess.run(
        ["ninja", "-C", str(build_dir), "-t", "commands", target],
        check=True, text=True, capture_output=True,
    )
    return result.stdout.splitlines()


def command_segments(line):
    """Split Ninja's simple command lists without invoking a shell."""
    segment = []
    for token in shlex.split(line):
        if token in ("&&", ";"):
            if segment:
                yield segment
            segment = []
        else:
            segment.append(token)
    if segment:
        yield segment


def build_arguments(build_dir, source):
    commands = ninja_commands(build_dir, "native-load-generator")
    compile_args = None
    compile_cwd = build_dir
    database = build_dir / "compile_commands.json"
    if database.is_file():
        for entry in json.loads(database.read_text()):
            directory = Path(entry["directory"])
            entry_source = (directory / entry["file"]).resolve()
            if entry_source == source:
                compile_args = entry.get("arguments") or shlex.split(entry["command"])
                compile_cwd = directory
                break
    if compile_args is None:
        for line in commands:
            for args in command_segments(line):
                if "-c" in args and (build_dir / args[args.index("-c") + 1]).resolve() == source:
                    compile_args = args
                    break
            if compile_args is not None:
                break
    if compile_args is None or "-o" not in compile_args:
        raise RuntimeError("Could not find the native-load-generator compilation command")

    object_path = (compile_cwd / compile_args[compile_args.index("-o") + 1]).resolve()
    for line in reversed(commands):
        for args in command_segments(line):
            if "-c" in args or "-o" not in args:
                continue
            for index, value in enumerate(args):
                if not value.startswith("-") and (build_dir / value).resolve() == object_path:
                    return compile_args, compile_cwd, args, index
    raise RuntimeError("Could not find the native-load-generator link command")


def compile_fixture(args, source, fixture_source, output_dir):
    compile_args, compile_cwd, link_args, object_index = build_arguments(args.build_dir, source)
    while Path(compile_args[0]).name in ("ccache", "sccache"):
        compile_args.pop(0)
    for option in ("-MT", "-MQ", "-MF"):
        while option in compile_args:
            index = compile_args.index(option)
            del compile_args[index:index + 2]
    compile_args = [value for value in compile_args if value not in ("-MD", "-MMD")]
    compile_args[compile_args.index("-c") + 1] = str(fixture_source)
    compile_args[compile_args.index("-o") + 1] = str(output_dir / "fixture.o")
    compile_args.insert(1, "-I" + str(source.parent))
    link_args[object_index] = str(output_dir / "fixture.o")
    link_args[link_args.index("-o") + 1] = str(output_dir / "fixture")
    metadata = {
        "source": str(source), "source_sha256": digest(source),
        "fixture_source_sha256": digest(fixture_source),
        "compile_cwd": str(compile_cwd), "compile": compile_args,
        "link_cwd": str(args.build_dir), "link": link_args,
    }
    (output_dir / "commands.json").write_text(json.dumps(metadata, indent=2) + "\n")
    environment = os.environ.copy()
    environment["CCACHE_DISABLE"] = "1"
    for phase, command, directory in (
        ("compile", compile_args, compile_cwd), ("link", link_args, args.build_dir),
    ):
        result = subprocess.run(command, cwd=directory, env=environment,
                                text=True, capture_output=True, timeout=args.timeout)
        (output_dir / (phase + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"{phase} failed; inspect {output_dir / (phase + '.log')}")


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=repository / "build")
    parser.add_argument("--output-dir", type=Path,
                        help="New or empty artifact directory (default: a retained temporary directory)")
    parser.add_argument("--timeout", type=int, default=600,
                        help="Maximum seconds for each compiler, linker, and test invocation")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    args.build_dir = args.build_dir.resolve()
    if not (args.build_dir / "build.ninja").is_file():
        parser.error("--build-dir must contain an existing Ninja build of native-load-generator")
    if args.output_dir:
        output_dir = args.output_dir.resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        if any(output_dir.iterdir()):
            parser.error("--output-dir must be empty")
    else:
        output_dir = Path(tempfile.mkdtemp(prefix="native-load-worker-regression-"))
    print(f"Artifacts: {output_dir}", flush=True)
    source = repository / "lite-client/native-load-generator.cpp"
    body = source.read_text()
    visibility = " private:\n  struct SignedTransfer {"
    entrypoint = "int main(int argc, char* argv[]) {"
    if body.count(visibility) != 1 or body.count(entrypoint) != 1:
        raise RuntimeError("Worker layout changed; update the fixture's explicit source boundaries")
    body = body.replace(visibility, " public:\n  struct SignedTransfer {", 1)
    body = body.replace(entrypoint, "int native_generator_original_main(int argc, char* argv[]) {", 1)
    template = Path(__file__).with_suffix(".cpp")
    fixture_source = output_dir / "fixture.cpp"
    fixture_source.write_text(body + "\n" + template.read_text())
    compile_fixture(args, source, fixture_source, output_dir)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([str(output_dir / "fixture")], text=True, capture_output=True,
                            timeout=args.timeout)
    (output_dir / "result.json").write_text(json.dumps({
        "returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr,
        "source_sha256": digest(source), "fixture_sha256": digest(template),
    }, indent=2) + "\n")
    print(result.stdout, end="")
    if result.returncode:
        print(result.stderr, file=sys.stderr, end="")
        raise RuntimeError(f"Worker regression failed with exit {result.returncode}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
