# Native CellDb durability probe

This probe attributes the serialized CellDb state-store path without changing its default durability policy. It is
disabled unless `TON_NATIVE_CELLDB_DURABILITY_PROFILE` is exactly `1`. Unset values, malformed values, and values such
as `true` preserve the original durability behavior: CellDb calls the no-argument `commit_write_batch()`, which uses
`rocksdb::WriteOptions::sync = true`, and it does not create a RocksDB statistics object when
`--disable-rocksdb-stats` is active. The instrumented binary retains a small flag branch and helper scaffolding even
when the profile is disabled, so performance comparisons must use the same image for all arms.

`TON_NATIVE_CELLDB_UNSAFE_SYNC_FALSE=1` requests the diagnostic ceiling. The request is honored only when
`TON_NATIVE_CELLDB_DURABILITY_PROFILE=1` is also present exactly; otherwise it is ignored and writes remain
synchronous. In the ceiling arm, CellDb uses `sync=false` while explicitly keeping `disableWAL=false`. The WAL is still
enabled, but RocksDB may acknowledge a write before its WAL bytes reach durable storage. A crash, power loss, or abrupt
runtime termination can therefore lose acknowledged state. Use this arm only on a disposable benchmark database. It
must never become production behavior.

The probe adds the following live `getstats` fields under `db.celldb`. Fields ending in `.micros` and backed by a
distribution use the existing `P50 / P95 / P99 / P100 / COUNT / SUM` format; all times are microseconds.

Detailed prepare, write-batch, commit, completion and `commit.calls` metrics cover successful, nonduplicate regular
state-store commits through `store_cell_impl`. Startup metadata repair, GC, migration and permanent-bulk commits use
the selected sync policy but do not contribute to those detailed samples. Queue-wait metrics have a broader
population: they include any regular or permanent-bulk store request that actually entered the serialized CellDb
queue, including a request that later resolves as a duplicate or error.

| Field suffix after `db.celldb.` | Meaning |
| --- | --- |
| `durability.enabled` | Always `true` when the profile is active; absent otherwise. |
| `durability.sync_writes` | Effective RocksDB `WriteOptions::sync` value. |
| `durability.wal_enabled` | `true`; the probe never disables the WAL. |
| `durability.periodic_reset_enabled` | Whether the ordinary 60-second RocksDB statistics flush also clears these probe accumulators. The physical benchmark disables that flush. |
| `durability.reset_generation` | Number of accumulator clears since CellDb startup. Before/after deltas are complete only when this value is unchanged. |
| `durability.queue_wait.micros` | Time spent by store requests that actually waited in CellDb's serialized action queue. Immediate requests are omitted. |
| `durability.queue_wait.count` | Number of store requests represented by the queue-wait distribution. |
| `durability.queue.max_depth` | Largest total CellDb action-queue depth observed when a store request was enqueued. |
| `durability.prepare.wall.micros` | Wall time from `prepare_commit_async` submission through its completion callback. |
| `durability.prepare.cpu.micros` | Sum of directly invoked prepare CPU and profiled async-executor CPU. It is exact for the current in-memory benchmark path and a lower bound for V1/V2. |
| `durability.prepare.caller_cpu.micros` | CPU consumed directly by the initial `prepare_commit_async` call. This is the full prepare CPU component for the current `--celldb-in-memory` path. |
| `durability.prepare.async_cpu.micros` | CPU consumed inside work submitted through the supplied async executor; normally zero for `--celldb-in-memory`. V1 actor continuations and V2's internally spawned helper threads are outside this counter. |
| `durability.write_batch.wall.micros` | Wall time to create and populate the RocksDB write batch, ending after `boc_->commit`; it excludes `DB::Write`. |
| `durability.write_batch.cpu.micros` | CPU for the same write-batch construction interval. |
| `durability.commit.wall.micros` | Caller-observed wall time of the blocking RocksDB `DB::Write` call. |
| `durability.commit.cpu.micros` | Caller-thread CPU consumed during that `DB::Write` call. |
| `durability.rocksdb.db_write.micros` | RocksDB's `DB_WRITE` histogram delta for the call. |
| `durability.rocksdb.wal_sync.micros` | RocksDB's `WAL_FILE_SYNC_MICROS` histogram delta for the call; zero in the sync-false arm. |
| `durability.completion.wall.micros` | Wall time from the incoming CellDb store request through scheduling result delivery, including queue wait, state construction, prepare, batch construction, commit, and snapshot publication. It does not wait for the downstream consumer to receive the promise. |
| `durability.commit.calls` | Number of attributed state-store commits. |
| `durability.write_batch.operations` | Sum of RocksDB records in attributed write batches. |
| `durability.write_batch.serialized_bytes` | Sum of serialized RocksDB write-batch bytes. |
| `durability.rocksdb.db_write.count` | RocksDB `DB_WRITE` histogram sample-count delta. |
| `durability.rocksdb.wal_write.count` | RocksDB `WRITE_WITH_WAL` ticker delta. |
| `durability.rocksdb.wal.bytes` | RocksDB `WAL_FILE_BYTES` ticker delta. |
| `durability.rocksdb.wal_sync.count` | RocksDB `WAL_FILE_SYNCED` ticker delta. |
| `durability.rocksdb.stall.micros` | RocksDB `STALL_MICROS` ticker delta. |

With ordinary RocksDB statistics enabled, interval statistics are flushed and reset on the existing schedule. With
`--disable-rocksdb-stats`, enabling this probe creates only the statistics object needed for per-commit attribution;
it does not enable periodic full RocksDB stats output, and the reported distributions accumulate for the process
lifetime.

Validator `getstats` reads CellDb's view from a cache refreshed every two
seconds. The harness snapshots also include its small setup and final-report
margins around the generator. A before/after delta can therefore include
margin activity and omit a small cache boundary interval even when
`reset_generation` is unchanged; treat it as a run-level diagnostic rather
than an exact canonical measurement-window counter.

Use three arms to interpret the result:

1. Leave both variables unset for the behaviorally unchanged, same-image baseline.
2. Set only `TON_NATIVE_CELLDB_DURABILITY_PROFILE=1` to measure attribution overhead and the normal sync-true path.
3. Set both variables to `1` on a disposable database to measure the sync-false ceiling.

Compare arm 2 with arm 1 before interpreting the probe. Compare arm 3 with arm 2 to bound the throughput and latency
available from removing the WAL durability wait. A negligible arm-3 gain rules out synchronous CellDb WAL persistence
as the useful next optimization target; a gain only identifies the cost ceiling and does not make sync-false safe to
ship.

The RocksDB wrapper now also exposes `flush_wal(sync)` as the durability
primitive needed by a future ordered cohort writer. A recovery test commits two
WAL-enabled `sync=false` batches, calls `flush_wal(true)` once, exits without
running database destructors, and verifies both records after reopening. No
production CellDb caller uses this primitive yet: ordinary writes remain
`sync=true`, and the unsafe ceiling retains its existing diagnostic semantics.
Introducing group commit still requires a bounded coordinator that preserves
write order and resolves every acknowledgement only after the shared WAL flush
succeeds.
