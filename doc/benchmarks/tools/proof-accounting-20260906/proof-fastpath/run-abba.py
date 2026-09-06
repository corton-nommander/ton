#!/usr/bin/env python3
"""Run a deferred offline A/B/B/A matrix with distinct preserved artifacts.

Invoke only after the parent confirms no builds or live workloads are active.
Both executables must have been prebuilt from the same driver and oracle.
Arguments following -- are forwarded identically to both executables.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', type=Path, required=True)
parser.add_argument('--candidate', type=Path, required=True)
parser.add_argument('--output-dir', type=Path, required=True)
parser.add_argument('driver_args', nargs=argparse.REMAINDER)
args = parser.parse_args()
forwarded = args.driver_args[1:] if args.driver_args[:1] == ['--'] else args.driver_args
baseline, candidate = args.baseline.resolve(), args.candidate.resolve()
for binary in [baseline, candidate]:
    if not binary.is_file():
        parser.error('missing prebuilt binary: ' + str(binary))
def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def checked_build_metadata(binary):
    sidecar = binary.with_suffix(binary.suffix + '.build.json')
    if not sidecar.is_file():
        parser.error('missing build metadata: ' + str(sidecar))
    metadata = json.loads(sidecar.read_text())
    if metadata.get('binary_sha256') != digest(binary):
        parser.error('prebuilt binary no longer matches build metadata: ' + str(binary))
    if metadata.get('output') != str(binary):
        parser.error('build metadata output path does not match selected binary: ' + str(binary))
    for field in ['driver_sha256', 'oracle_sha256']:
        value = metadata.get(field)
        if not isinstance(value, str) or len(value) != 64 or any(c not in '0123456789abcdef' for c in value):
            parser.error('missing/invalid ' + field + ' in ' + str(sidecar))
    return {'path': str(binary), 'sha256': metadata['binary_sha256'],
            'build_metadata_path': str(sidecar), 'build_metadata_sha256': digest(sidecar),
            'build_metadata': metadata}

baseline_provenance = checked_build_metadata(baseline)
candidate_provenance = checked_build_metadata(candidate)
for field in ['driver_sha256', 'oracle_sha256']:
    if baseline_provenance['build_metadata'][field] != candidate_provenance['build_metadata'][field]:
        parser.error('baseline and candidate were built with different ' + field)
output_dir = args.output_dir.resolve()
if output_dir.exists() and any(output_dir.iterdir()):
    parser.error('output directory is not empty; existing runs are never overwritten')
output_dir.mkdir(parents=True, exist_ok=True)
metadata = {'schema': 'native-proof-offline-abba-v1', 'driver_arguments': forwarded,
            'baseline': baseline_provenance,
            'candidate': candidate_provenance,
            'semantics': 'Offline component cost only; copy/proof exclude dictionary construction, verification, and destruction. No TPS or capacity claim.',
            'runs': []}
metadata_path = output_dir / 'provenance.json'
for label, binary in [('a1', baseline), ('b1', candidate), ('b2', candidate), ('a2', baseline)]:
    csv_path, log_path = output_dir / (label + '.csv'), output_dir / (label + '.stderr')
    start = time.monotonic()
    with csv_path.open('x') as stdout, log_path.open('x') as stderr:
        completed = subprocess.run([str(binary), *forwarded], stdout=stdout, stderr=stderr)
    with csv_path.open() as stream:
        rows = list(csv.DictReader(stream))
    metadata['runs'].append({'label': label, 'command': [str(binary), *forwarded],
        'exit_code': completed.returncode, 'wall_seconds': time.monotonic() - start,
        'csv_sha256': digest(csv_path), 'rows': len(rows),
        'timed_checkpoint_stat_comparisons': sum(int(row['checkpoints']) * int(row['iterations']) for row in rows)})
    metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
    print(label + ': exit ' + str(completed.returncode) + ', ' + str(len(rows)) + ' rows', flush=True)
    if completed.returncode:
        raise SystemExit(completed.returncode)
