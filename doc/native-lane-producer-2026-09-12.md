# Persistent native lane producers — experimental

`TON_NATIVE_PERSISTENT_PRODUCER=1` moves callback selection and bounded queue
publication into persistent producer actors for fixed basechain payment lanes
(depth 1–6). Its code default remains **off**. The completed strict-image stage2
comparison retained it for subsequent benchmark profiles; this does not change
the default or establish production capacity. Broad checkpoint retention remains off.

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
native view updates and generic externals. The separately implemented
[four prepared lane owners](native-prepared-lane-owners-2026-09-12.md) can own
these publications; their activation and performance decision remain separate.

The frozen `52a20df0` implementation passed **144 focused cases**, the native
publication gate's **21 invocations**, and all **seven frozen-runtime target
checks**. These are recorded executions, not newly rerun tests. The exact binary,
test and log hashes are in
[integration verification](../build/benchmarks/native-lane-pipeline-20260912/integration-verification-52a20df0.json)
and the [frozen runtime gates](../build/benchmarks/native-lane-pipeline-20260912/frozen-runtime-gates-52a20df0.json).
The later split-startup build `095bc0de` repeated the 144 focused cases,
21 publication-gate invocations and seven frozen-runtime checks. Its existing
mock-manager ordinary-idle regression also passed; a real fresh zero-ingress
split transition remains a separate pending gate. See the
[cycle report](native-lane-pipeline-cycles-2026-09-12.md#validation-and-next-context).

Performance result: four proof-complete 600-second stage2 ABBA arms produced
mean canonical native-transfer rates of **55,891.51 → 57,135.33 TPS (+2.225%)**.
Both treatment repeats exceeded both controls, so the producer was retained for
subsequent tests. The gain approximately matched the increase in sampled validator
CPU; it is **not a CPU-efficiency or capacity claim**. See the
[stage2 comparison](../build/benchmarks/native-lane-pipeline-20260912/stage2-comparison.json)
and [12 September cycle report](native-lane-pipeline-cycles-2026-09-12.md).

## Selected benchmark reference

Pending the corrected ownership result, retain producer1/preparation0/journal0
with owners0/domains0 and broad checkpoint retention0. Keep four lanes, 20ms
coalescing and a 65,536 maximum logical window for matched follow-up tests; these
are the measured desktop context, not a new production hardware optimum. The
owner experiment needs preparation1 on both sides, so an owner improvement alone
would still need a complete-profile comparison against this reference.

An empty-shard startup diagnosis found that native idle suppression can delay
structural split candidates. Readiness eventually completed without a patch.
The split-only fix is committed as `095bc0de`; real zero-ingress verification
and any performance result remain pending. It excludes after-merge transitions;
keep proof-checked four-lane readiness separate from the measured load window.
