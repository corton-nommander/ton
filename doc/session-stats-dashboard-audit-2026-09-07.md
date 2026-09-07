# Session Stats canonical TPS audit — 2026-09-07

**The running dashboard's “Canonical transactions per second” → “Workchain” series correctly shows 60k+ logical TPS for the completed ADNL tests. The separate “Canonical native transfers per second” series is broken for NTRN signed runs and reports zero.**

The audit queried the actual running image `sha256:5f3576b583a98779069ea7e4900db2b3d74289aa99b230414c5226c1db792cb0`, inspected its deployed backend/frontend, and compared historical API data with the saved proof-checked generator results. No new load was submitted and no service was changed. The older local `session-stats` checkout does not match the deployed native implementation.

## Numerical reconciliation

| Submission connections | Dashboard peak 1-minute canonical TPS | Generator measured canonical TPS | Dashboard total = independently proven generator total |
| ---: | ---: | ---: | ---: |
| 10 | 64,873.60 | 62,800.80 | 15,492,864 |
| 50 | 63,591.73 | 63,433.12 | 15,465,040 |
| 100 | 63,615.20 | 60,250.28 | 15,458,320 |

**All three full-run totals match exactly: 46,416,224 logical transfers.** The totals include warm-up and drain. The API queries cover isolated whole-minute ranges around each arm; intervening boundary buckets contain zero workchain transfers. This comparison does not divide a padded whole-run total by the measured 180 seconds.

The dashboard aggregates consensus-acceptance-time buckets, summing all four workchain-0 lanes and dividing by 60 for its 1-minute view. It correlates accepted candidates with masterchain-selected shard ancestry and deduplicates by full block ID across repeated imports. This is canonical evidence from validator logs; the generator independently verifies canonical blocks and source/message attribution using liteserver proofs.

The generator's reported average uses 179 fully contained block `gen_utime` seconds inside its 180-second measurement period. Different timestamps, boundaries and averaging periods explain why a dashboard minute peak is higher than the measured-run average. The dashboard's 63,615 minute peak in the 100-client arm is consistent with that arm's 60,250 average; it is not a new sustained-capacity result.

## Which chart to use

Select **“Canonical transactions per second”**, legend **“Workchain”**, and **Window size: 1m**. The page defaults to the last two hours, so choose the historical test range when reviewing older results. It has no automatic refresh timer: reload or change the time range to fetch new data, allowing for importer delay. Larger windows average peaks over a longer period; the backend can also coarsen very long ranges.

The canonical chart uses:

```text
/api/stats_single?stat=BLOCK_APPLIED_transactions&mode=rate&window_size=60
```

For the inspected historical interval:

```sh
curl --fail --get http://127.0.0.1:18000/api/stats_single \
  --data-urlencode 'stat=BLOCK_APPLIED_transactions' \
  --data-urlencode 'mode=rate' \
  --data-urlencode 'window_size=60' \
  --data-urlencode 'start=2026-09-06T17:17:00Z' \
  --data-urlencode 'end=2026-09-06T17:40:00Z'
```

“Mean per-block native collation service rate (transfers/s)” and the analogous validation chart measure processing speed within a block, not canonical chain TPS. They must not be used as capacity results. The native-specific series were zero/empty in the audited deployment because of the classification bug below.

## Confirmed native-specific importer bug

Deployed `/app/backend/updater.py:223–251` falls back to classifying a gas-free block as native only when:

```python
ext_msgs_accepted == transactions
```

That condition describes scalar transfers. NTRN records count physical accepted parents separately from logical transfers. One real block in the 100-client run has **2,528 transactions, 158 accepted parents, zero gas**; 158 × 16 = 2,528. Neither explicit `native_transfers` nor `native_transactions` is currently present in that block's stats. The importer returns no native count, so it omits nonempty native-specific samples. Empty blocks can still emit zero samples.

`BLOCK_APPLIED_transactions` always uses the already-logical `transactions` count and therefore remains correct. There is no additional ×16 multiplier in the verified chart. The same classification issue applies to equivalent NTRN messages submitted through ADNL, tonlib or lite-client: Session Stats reads validator outcomes, not the client API. The high-rate evidence here is from persistent ADNL; tonlib was separately integration-tested at low volume, not benchmarked at 60k.

The bug is documented, **not fixed in the running service**. A robust fix should preserve an explicit logical native count or otherwise support source-signed runs without assuming every parent has exactly 16 children. Regression coverage should include scalar messages, full runs, short tails, and canonical deduplication. Previously omitted historical native samples would also need controlled reimport; merely upgrading the parser will not necessarily repair rows protected by already-processed block markers.

[Machine-readable evidence](benchmarks/results/session-stats-dashboard-audit-20260907.json) contains the API series, chart configuration, runtime source hashes, the counterexample and all three exact-total comparisons. The [remote client guide](native-remote-client-guide.md) now identifies the chart to use for this deployed image.
