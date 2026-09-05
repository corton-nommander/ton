# Staged ShardAccounts trie worker benchmark

Build the explicit benchmark target from a Release configuration:

```sh
cmake --build build --target bench-staged-trie --parallel 4
```

The target is excluded from normal builds and CTest. It does not alter the
production parallel threshold, which remains 512 staged account updates.

Finish builds and stop load generation before recording timings. Reuse the
same binary for both runs, and record its SHA-256 and the host/compiler details.
The benchmark launches at most eight workers per operation. A bounded initial
measurement covering every case is:

```sh
build/bench-staged-trie --rounds 5 --iterations 5 > staged-trie-1.csv
build/bench-staged-trie --rounds 5 --iterations 5 > staged-trie-2.csv
```

The defaults (`--rounds 7 --iterations 20 --base-accounts 4096`) provide longer
sampling. `--base-accounts` can be increased independently to test larger prior
state; it must be at least 512. Report each run separately before aggregating,
and pair each worker tier with the one-worker sample in the same round.

Each run covers 168 cases:

- 64, 80, 128, 192, 256, 384, and 512 unique account updates.
- 1, 2, 4, and 8 workers.
- Uniform 256-bit addresses, the current two-bit payment-lane prefix, and a
  separate sixteen-bit shared-prefix stress workload.
- Plain prior state and prior state wrapped in a fresh `CellUsageTree` for each
  operation. Tracked prior-state traversal must remain serial in production.

Fixtures contain 4,096 existing native accounts by default. Each operation
replaces 75% of its keys and inserts 25%. Addresses, balances, and nonces are
deterministic, and every tier starts with the same prior state and sorted
updates. The benchmark builds native Account cells through the production
builder, computes the expected root with sequential `set`, checks every measured
result against that root, and fully validates the first warmup for every tier.

Each CSV row averages the requested iterations. Wall time uses `steady_clock`;
CPU time uses `std::clock` (process CPU including workers on Linux). Both cover
only `set_many_sorted_parallel`, matching the production staged dictionary
timer. Setup, validation, and result destruction happen outside the timer.
There are two warmups per case/tier; worker order rotates between paired rounds.
Use median paired speedup (`one_worker_wall / tier_wall`), individual run
medians, and CPU cost when assessing a tier.

The one-worker tier invokes the existing serial bulk path. Higher tiers also
switch to direct sorted update-trie construction when values permit it, so the
comparison includes that algorithmic difference as well as thread count. A
microbenchmark speedup is not a TPS measurement. The plain-state case can merge
in parallel; the tracked-state case preserves the serial prior-state merge.
The sixteen-bit prefix is a stress case, not the production lane topology.

Before lowering the threshold, correlate these measurements with production
histograms of both microbatch unique accounts and actual staged checkpoint
updates. Coalescing can make those populations different. The buckets should
distinguish the previous maximum of 80 and the exact parallel boundary of 512,
and worker counts should identify whether parallel staging actually occurred.
A full capacity claim additionally needs repeated paired validator runs using
prebuilt strict image reuse and offered load above canonical throughput.

## Measurements on 2026-09-05

Two complete 5-round, 5-iteration runs used the same Release binary on an AMD
Ryzen 9 3900X (Clang 22, `-O3 -DNDEBUG -march=native`). Builds/tests had finished;
genesis preparation was paused and no validator or generator ran during the
measurements. All 8,400 timed operations and 672 warmups passed root equality;
the first warmup in each case/tier also passed full dictionary validation.
Elapsed times were 12.05 and 12.07 seconds. The desktop was not otherwise
isolated or frequency-pinned, so close tier differences need longer sampling.

[Raw run 1](results/staged-trie-20260905-1.csv),
[raw run 2](results/staged-trie-20260905-2.csv),
[all distributions and per-run medians](results/staged-trie-20260905-summary.csv),
and [binary/source hashes and host metadata](results/staged-trie-20260905-metadata.json)
preserve the measurements. The table below shows median wall microseconds per
operation across the ten round samples for the two-bit payment-lane workload.

| Prior state | Updates | 1 worker | 2 workers | 4 workers | 8 workers |
| --- | ---: | ---: | ---: | ---: | ---: |
| plain | 64 | 740.6 | 1800.8 | 1670.5 | 1912.4 |
| plain | 80 | 910.0 | 1855.7 | 1732.0 | 1942.2 |
| plain | 128 | 1366.8 | 2196.1 | 1944.2 | 2088.2 |
| plain | 192 | 2050.2 | 2598.2 | 2199.0 | 2275.8 |
| plain | 256 | 2659.7 | 2961.6 | 2483.9 | 2488.4 |
| plain | 384 | 3895.8 | 3833.2 | 2971.4 | 2838.8 |
| plain | 512 | 5001.2 | 4336.4 | 3415.5 | 3309.3 |
| tracked | 64 | 816.6 | 736.7 | 746.8 | 876.9 |
| tracked | 80 | 991.8 | 837.0 | 849.9 | 972.8 |
| tracked | 128 | 1493.4 | 1200.4 | 1215.8 | 1330.8 |
| tracked | 192 | 2200.5 | 1715.7 | 1673.3 | 1758.6 |
| tracked | 256 | 2835.0 | 2078.8 | 2104.9 | 2115.1 |
| tracked | 384 | 4094.5 | 2815.9 | 2741.6 | 2789.6 |
| tracked | 512 | 5261.6 | 3527.1 | 3347.1 | 3301.0 |

The prior-state graph changes the result substantially. At the observed
80-account size, two workers on tracked two-bit state delivered a median paired
1.18x wall-time speedup and used 0.91x process CPU. On plain two-bit state the
fastest parallel tier (four workers) took 1,732 microseconds versus 910 for one
worker, so applying that tier globally would regress this case.

Across all three address distributions, one worker was fastest for plain state
at 64–192 updates in both runs, while eight workers won at 384 and 512. For
tracked state, two workers usually led at 64–128 and four at 192–384; the best
tier at 512 depended on distribution. Several close results at 128, 256, and
512 changed winner between runs. These are measured staging costs, not TPS
gains, and include the serial/direct-construction algorithm difference noted
above.

The production threshold remains 512. Production account-count histograms
were added for microbatches and staged checkpoints with disjoint buckets
`<=64`, `65–80`, `81–128`, `129–256`, `257–511`, `512`, and `>512`, plus worker
counts. These count attempts, including work later rolled back. Before a
policy change, collect those histograms under representative offered load and
check which prior-state graph shapes occur; a size-only threshold cannot infer
the plain/tracked difference demonstrated here.

## Production population from the 39k screening diagnostic

The measured treatment window in
`recovery-20260905-treatment1-39k-60s/validator-pipeline-summary.json` contains
681 basechain candidate records across 60 seconds. Histogram capture is
complete and its counts reconcile. The
[preserved production excerpt and source hashes](results/staged-trie-production-20260905-treatment1-39k.json)
record the independently checked source values.

| Unique accounts or staged updates | Microbatch attempts | Staged checkpoint attempts |
| --- | ---: | ---: |
| <=64 | 8,909 | 7,249 |
| 65–80 | 0 | 26 |
| 81–128 | 0 | 87 |
| 129–256 | 0 | 379 |
| 257–511 | 0 | 0 |
| 512 | 0 | 0 |
| >512 | 0 | 0 |
| Total | 8,909 | 7,741 |

Every staged checkpoint used one worker: 7,741 one-worker attempts and zero
2/4/8/other-worker attempts. The maximum unique accounts in a microbatch was
64. Coalescing produced 492 checkpoints above 64 updates, including 379 in the
129–256 bucket; the histogram does not establish their exact maximum. No
checkpoint reached 257 updates. The 512-update parallel threshold was therefore
still dormant in this production screening population and remains unchanged.
These counters describe attempts, including work that could later roll back.

This is a screening diagnostic. The paired 39k control failed ingress acceptance
with `offer_target_not_attained`, failed chain-capacity acceptance with
`insufficient_load_over_canonical_throughput`, and was classified `rejected`.
The population data is useful for choosing future trie workloads, but this pair
supports no capacity or TPS-gain claim. It also does not identify the measured
prior-state graphs as plain or tracked. Valid paired throughput evidence and
that graph distinction are still needed before changing the worker policy.
