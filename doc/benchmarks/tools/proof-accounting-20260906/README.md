# Proof-accounting experiment tools, 2026-09-06

These seven files preserve the exact bytes used by the offline proof experiment
and completed live-series analysis. [manifest.json](manifest.json) records each
SHA256, byte count, mode, and original repository-relative location. The frozen
driver and oracle match both component build sidecars; the component analyzer
and live audit match their executed-result hashes. No binaries or logs are
included, and these tools are not registered in production CMake or CTest.

| Files | Purpose |
| --- | --- |
| `proof-fastpath/bench-native-proof.cpp`, `reference-cell-storage-stat.h` | Offline driver and independent frozen legacy accounting oracle. |
| `proof-fastpath/build-driver.py` | Compile/link the driver using an existing configured `bench-staged-trie` toolchain; prints commands unless `--execute` is supplied. |
| `proof-fastpath/run-abba.py`, `analyze-abba.py` | Execute preserved A1/B1/B2/A2 component matrices and validate/analyze their complete provenance and CSVs. |
| `audit-proof-series.py` | Audit the fixed four-arm live plan, hashes, capacity gates, settings, images, mounts, and process continuity. |
| `analyze-proof-arm.py` | Read one completed live bundle and emit checkpoint timing and packing analysis to stdout. |

The offline baseline uses `68900d7f31f7772440ccb57be87b5fabf07e3166`; the refined
candidate source is `13ac90e8486eeca6466dd629b4d9e66c5a3cbd83`. The candidate
component sidecar records the earlier HEAD `d68388d9`, because compilation preceded
the candidate commit. Its captured `crypto/vm/boc.cpp` SHA256 matches `13ac90e8`.
The baseline sidecar predates the source-revision/hash fields. Preserve those
original records; use the source hashes and revisions in the manifest when
reconstructing the libraries.

The live candidate image uses `6a96c953508844b97447b3f7fc6b0e25ad62fcd9`, whose
proof source matches `13ac90e8`; its control is the incumbent `4caa92df`. Frozen
service-image IDs and all live settings are in the committed
[live plan](../../results/cycles-20260906b-proof-live-plan.json). The live audit
reads that plan from commit `50b175f5`. Component improvements do not establish
TPS gains: all four live arms missed the ingress target gate, so the incumbent
remains selected.

Restore the files to their original locations before use. Some tools infer the
repository root or find neighboring files from their own location. From the
repository root, this copies only verified archive bytes and refuses to replace
a different existing file:

```bash
python3 - <<'PY'
import hashlib, json, shutil
from pathlib import Path
root = Path.cwd()
archive = root / 'doc/benchmarks/tools/proof-accounting-20260906'
entries = json.loads((archive / 'manifest.json').read_text())['files']
for entry in entries:
    source = archive / entry['archive_relative_path']
    target = root / entry['original_relative_path']
    data = source.read_bytes()
    if len(data) != entry['bytes'] or hashlib.sha256(data).hexdigest() != entry['sha256']:
        raise SystemExit(f'Archive hash/size mismatch: {source}')
    if target.exists() and target.read_bytes() != data:
        raise SystemExit(f'Existing file differs: {target}')
for entry in entries:
    source = archive / entry['archive_relative_path']
    target = root / entry['original_relative_path']
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
PY
```

Use Python 3.11 or later. For a new component experiment, build the intended TON
libraries in separate configured Ninja build trees at the baseline and candidate
revisions, with matching compiler/build settings and the existing
`bench-staged-trie` target available. The archived builder does not build those
libraries. Invoke it once per build tree with distinct absolute output paths;
preserve each executable and its `.build.json` sidecar at their recorded paths:

```bash
python3 build/benchmarks/cycles-20260906b/proof-fastpath/build-driver.py \
  --build-dir /absolute/baseline/build --output /absolute/output/bench-proof-baseline --execute
python3 build/benchmarks/cycles-20260906b/proof-fastpath/build-driver.py \
  --build-dir /absolute/candidate/build --output /absolute/output/bench-proof-candidate --execute
```

Run component timing without concurrent builds, tests, or live benchmarks. The
executed production matrix used the following arguments; use new output
directories to retain all prior evidence:

```bash
python3 build/benchmarks/cycles-20260906b/proof-fastpath/run-abba.py \
  --baseline /absolute/output/bench-proof-baseline \
  --candidate /absolute/output/bench-proof-candidate \
  --output-dir /absolute/output/abba-production -- \
  --rounds 5 --iterations 5 --base-accounts 12288 --base-proof-leaves 64 --mode tracked_prior
python3 build/benchmarks/cycles-20260906b/proof-fastpath/analyze-abba.py \
  --input-dir /absolute/output/abba-production --output-dir /absolute/output/abba-production-analysis
```

The smaller screen used `--rounds 3 --iterations 3 --base-accounts 4096
--base-proof-leaves 64 --mode all`. Exact compiler/link commands, binary/library
hashes, matrix arguments, and raw CSV hashes remain in the
[screen provenance](../../results/cycles-20260906b-proof-v2-screen-provenance.json)
and [production provenance](../../results/cycles-20260906b-proof-production-provenance.json).
New builds need not reproduce original binary bytes; keep their new provenance.

For completed live bundles, run `analyze-proof-arm.py /absolute/bundle` and redirect
stdout to a new JSON file. Run `audit-proof-series.py --output /absolute/new-audit.json`
only after all four raw bundles, saved result JSONs, and launch-command JSONs are
available at their original locations, or supply its documented path overrides.
It exits 0 for an eligible series, 3 for rejected/incomplete evidence, and 2 for
input/output errors; it never overwrites an audit output. These live tools do not
launch Docker or load. Missing raw bundles cannot be reconstructed from summaries
alone. Neither analysis claims statistical significance or selects a batching
default.
