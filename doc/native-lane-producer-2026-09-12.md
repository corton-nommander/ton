# Persistent native lane producers — experimental

`TON_NATIVE_PERSISTENT_PRODUCER=1` moves callback selection and bounded queue
publication into persistent producer actors for fixed basechain payment lanes
(depth 1–6). It remains **off by default**, pending the recorded correctness and
600-second strict-image comparisons. Broad checkpoint retention remains off.

The admission owner publishes immutable committed source views, containing
external-message references and copied interval/priority/deadline facts. No
mutable reservation, account map, message-liveness object or broadcast promise
is shared. Canonical progress is published through read-only producer fences;
only the admission owner can write them. Each callback retains its own exact
parent floors, next nonces, exclusions, delivered hashes and queue epochs.
Selection starts at the maximum of its canonical fence and exact-parent floor.

A persistent source index includes committed suffixes even when the canonical
head is missing or uncommitted: an exact parent may legitimately begin after
that head. Ordinary, uniform-priority ingress is traversed lazily. Mixed-priority
work retains the original eager priority/address ordering to prevent starvation
of a low-priority head hiding a higher-priority suffix. Mode/topology transitions
retire old producers before reservation changes; unsupported topology and
synchronous snapshots retain the original pool path.

Owner publication and producer ingestion each process at most 256 sources per
turn. Each inbox coalesces replacements by source and permits at most 65,536
pending source views; excess work remains as owner-side dirty keys until space
is available. Announced, incomplete publication keeps the producer epoch open
between chunks; a callback cannot mistake a partial refresh for exhaustion.
A canonical account revision change, erase or postponement fences
an old view before the corresponding reservation walk completes. Producer
reactivation events are bounded to one next deadline per source. Existing initial
transport-window publication, 512-logical-transfer selection fragments, atomic
signed runs, queue accounting, cancellation and completion markers are retained.

`total.ext_msg_native_persistent_producer` reports publication and source counts,
coalescing, inbox occupancy, index mutations, installs, probes, invalidated
probes, selected physical/logical work and aggregate publication/install/wait
microseconds. The standard native transport counters combine all producers.
These counters describe scheduling work, not canonical TPS.

The first implementation reuses the tested scheduler/pump implementation in a
restricted producer-only `ExtMessagePool` actor. This instance starts no
signature workers and owns no admission/canonical account state. Collator queue
installation still passes through the admission owner to preserve ordering of
native view updates and generic externals. Direct routing and independent
admission owners are separate later experiments.

Validation status: differential and real actor/lifecycle test translation units
pass syntax checks; full execution and throughput comparison remain pending. Benchmark
artifacts belong under `build/benchmarks/native-lane-pipeline-20260912` and will be
summarized in the cycle report after proof-complete measurements.
