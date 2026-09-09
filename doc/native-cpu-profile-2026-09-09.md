# Validator CPU profile — 9 September 2026

The 60-second capture is valid: immutable validator/container identity and
executable SHA matched before/after; 53,293 software `cpu-clock:u` samples,
549.41 sampled user-CPU seconds, and zero lost samples. The original report
failed with exit 141 during addr2line setup. Offline reports regenerated with
`--no-inline` succeeded against the same executable and copied mapped DSOs;
the original failure and data remain intact. No second capture ran.

This diagnostic used the frozen f5978 runtime, cache=1, sharing=0, refresh=0,
four lanes, 10 connections and 20 ms coalescing. The separate 180-second load
passed the parent's proof/drain/strict-reuse checks and is excluded from TPS
comparisons because profiling adds overhead.

| Observed CPU work | Share of sampled user CPU | Interpretation |
| --- | ---: | --- |
| Overlay peer signature checks | 14.274% | Every observed caller was a successful local signing callback: FEC 8.763%, simple 5.511%. Investigate avoiding this redundant cryptographic check while retaining every other check and all incoming-message verification. |
| Keyring private-key import/public-key derivation | 4.533% | Repeated `EVP_PKEY_new_raw_private_key` work; prepared immutable key reuse is a bounded candidate. Actual signing separately accounts for 4.408%. |
| Candidate native-message decoding | 11.322% | StateResolver decode/hash/cell construction warrants exact-candidate decoded-data reuse investigation. |
| ExtMessagePool, excluding verifier actors | 9.333% | About 0.85 sampled core in a serialized actor; long turns can constrain delivery latency despite spare aggregate CPU. |
| Reconciliation walk / target registration | 2.381% / 0.259% | Bounded yielding is a latency experiment; its direct CPU savings alone are smaller than the work above. |
| Native admission signature verification | 5.571% | Crypto-only attribution; the existing complete-payload positive cache already serves this path. |

Component rows overlap and must not be summed. None is a measured TPS gain.
Exclusive instruction-pointer leaders are `fe_mul` 14.74%, `fe_sq` 7.19%,
`DataCell::create` 4.61%, SHA256 compression 3.86%, `DataCell` destruction 3.25%,
and `CellSlice::prefetch_ref` 3.13%.

The enclosing 62.863-second resource window used 10.15 mean genesis CPU cores
(9.34 user + 0.81 system) against a 16-core quota. No additional cgroup CPU
throttling occurred. Storage reads were zero; genesis wrote 3.519 GB,
55.98 MB/s and 320.65 write operations/s. Memory events and pressure remained
zero; cgroup memory increased from 24.12 to 26.42 GB. Whole-VM I/O pressure
was 3.74% some / 3.33% full, and CPU pressure 1.73% some; these include other
containers and the profiler and cannot be assigned solely to genesis.

Unwind limits: 9.95% of samples lack a named leaf, mostly stripped libraries;
75.98% reach an identifiable thread root and 1,247/53,293 have at most two
frames. No lost-sample warning or decoder error occurred. Async actor messages
cannot be joined into synchronous stacks. There were 4,731 fork/exit events;
97 Hz sampling can underrepresent short-lived signature helper threads, so the
small sampled helper share does not quantify thread creation or joining costs.

Artifacts: `cpu-analysis.json`, `capture-snapshot-analysis.json`, original
`run.json`, `identity-before.json`, `identity-after.json`, `perf.data`, and
`report-recovery/{recovery.json,report-self-symbols.txt,report-callgraph.txt,
perf-script.txt,stack-analysis.json}`. The file `report-recovery/report-self.txt`
contains event statistics only; use `report-self-symbols.txt` for exclusive CPU.

Full raw artifacts are under `build/benchmarks/admission-cycles-20260909/cpu-profile-prep/results/20260909T065210Z-capture-2da749b6/`. Compact evidence is in [native-cpu-profile-20260909.json](benchmarks/results/native-cpu-profile-20260909.json).
