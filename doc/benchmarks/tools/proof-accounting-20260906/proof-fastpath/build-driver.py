#!/usr/bin/env python3
"""Deferred standalone build using the existing bench-staged-trie toolchain.

Defaults to printing commands. --execute compiles/links only this driver; it
never rebuilds the TON libraries, modifies CMake, or starts a workload.
Build the intended baseline/candidate ton_crypto libraries first, while idle.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--execute', action='store_true')
args = parser.parse_args()
build_dir = args.build_dir.resolve()
output = args.output.resolve()
source = Path(__file__).resolve().with_name('bench-native-proof.cpp')
object_path = output.with_suffix(output.suffix + '.o')
if output.exists() or object_path.exists():
    parser.error('output/object already exists; choose a new baseline or candidate artifact name')
commands = subprocess.check_output(['ninja', '-t', 'commands', 'bench-staged-trie'], cwd=build_dir, text=True).splitlines()
compile_lines = [line for line in commands if 'bench-staged-trie.cpp' in line and ' -c ' in line]
link_lines = [line for line in commands if 'bench-staged-trie.cpp.o' in line and ' -o bench-staged-trie ' in line]
if len(compile_lines) != 1 or len(link_lines) != 1:
    parser.error('could not identify exactly one existing benchmark compile and link command')
compile_command = shlex.split(compile_lines[0])
compile_command[compile_command.index('-c') + 1] = str(source)
compile_command[compile_command.index('-o') + 1] = str(object_path)
for flag, value in [('-MF', str(object_path) + '.d'), ('-MT', str(object_path))]:
    if flag in compile_command:
        compile_command[compile_command.index(flag) + 1] = value
link_command = shlex.split(link_lines[0])
if link_command[:2] == [':', '&&']:
    del link_command[:2]
if link_command[-2:] == ['&&', ':']:
    del link_command[-2:]
objects = [index for index, value in enumerate(link_command) if value.endswith('/bench-staged-trie.cpp.o')]
if len(objects) != 1:
    parser.error('could not identify benchmark link object')
link_command[objects[0]] = str(object_path)
link_command[link_command.index('-o') + 1] = str(output)
for command in [compile_command, link_command]:
    if any(token in {'&&', '||', ';', '|'} for token in command):
        parser.error('unexpected shell operation in build command; inspect manually')
    print(shlex.join(command))
if not args.execute:
    raise SystemExit(0)
output.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(compile_command, cwd=build_dir, check=True)
subprocess.run(link_command, cwd=build_dir, check=True)

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

metadata = {'output': str(output), 'binary_sha256': digest(output),
            'driver_sha256': digest(source),
            'oracle_sha256': digest(source.with_name('reference-cell-storage-stat.h')),
            'compile_command': compile_command, 'link_command': link_command,
            'build_dir': str(build_dir), 'library_sha256': {}}
for library in ['crypto/libton_crypto.a', 'crypto/libton_crypto_core.a', 'crypto/libton_block.a']:
    metadata['library_sha256'][library] = digest(build_dir / library)
for line in (build_dir / 'CMakeCache.txt').read_text().splitlines():
    if line.startswith('CMAKE_HOME_DIRECTORY:INTERNAL='):
        source_dir = Path(line.split('=', 1)[1])
        metadata['production_boc_sha256'] = digest(source_dir / 'crypto/vm/boc.cpp')
        metadata['source_revision'] = subprocess.check_output(
            ['git', 'rev-parse', 'HEAD'], cwd=source_dir, text=True).strip()
        break
output.with_suffix(output.suffix + '.build.json').write_text(json.dumps(metadata, indent=2) + '\n')
