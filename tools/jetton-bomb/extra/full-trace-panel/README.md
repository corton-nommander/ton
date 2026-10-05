# Jetton Transfer time — Grafana panel for toncenter's Analytics dashboard

(The panel is titled **Jetton Transfer time**; the metric it plots is still called
`full_trace_time`.)

Measures how long a monitor-sent message actually takes, from two independent angles:

- **`finalized_action_in` (streaming)** — what toncenter's streaming monitor itself reports, from
  the `dm_<net>` streaming table.
- **`full_trace_time` (on-chain)** — the tonscan-style trace duration, last tx minus first tx of the
  trace, computed from the v3 indexer's `<net>_d.transactions`.

The gap between the two is the point: on-chain the trace may already be done while streaming has
not reported it yet. Built during the 31.08.2026 testnet max-load run, where streaming finalized
only ~13.6% of probes within the hour and lagged 8–11 min.

## Networks

`make_local.py` builds one dashboard per network from the same design; they differ only in
databases, the monitor's sender wallet and the datasource uids.

| | testnet | mainnet |
|---|---|---|
| dashboard uid | `fulltracetimetestnet` | `fulltracetimemainnet` |
| streaming | `dm_testnet.toncenter_streaming_testnet` | `dm_mainnet.toncenter_streaming_mainnet` |
| on-chain | `testnet_d` | `mainnet_d` |
| monitor wallet | `0:CCA3DC49…A1381` | `0:086CA3CD…4AEA` (`EQAIbKPNrlKWAvnJwTgmSB153yRzhRsKm06jNKv2H0tK6iQ1`) |
| probe cadence | 40 s | 120 s |

The cadence difference needs no configuration: bar width follows from the real gap between
messages, which is the whole reason the series are joined on the message hash instead of being
quantized onto a time grid.

## Layout

```
make_local.py                  local preview dashboards -> grafana/dashboards/full_trace_<net>.json
make_json.py                   hand-off files           -> full_trace_panel_<net>.json
full_trace_panel_testnet.json  what the toncenter dashboard admin pastes into uid af5z8mnmm15hcb
full_trace_panel_mainnet.json  the same for the mainnet Analytics dashboard
grafana/                       provisioning for the local preview container
```

Each `full_trace_panel_<net>.json` carries several pieces, so the admin can pick what fits his access:

- `variant_i` — one query, one datasource, cross-DB `LEFT JOIN`. Needs a ClickHouse user with
  grants on **both** the `dm_*` and the `*_d` database. No such user exists on our cluster, so this
  variant is correct by construction but was never run end to end.
- `variant_ii_two_datasource_DEPLOYABLE` — two targets on two datasources, joined in Grafana. This
  is what the local preview runs.
- `modified_table_panel_id6` — the existing "TX hashes" table with a `full_trace_time` column added.
- `table_column_addition__left_join_block` — just the join block, if he prefers to patch by hand.

## Running the local preview

```bash
export CH_DM_PASSWORD=...        # dm_testnet
export CH_VICREAD_PASSWORD=...   # testnet_d
docker run -d --name ftt-grafana -p 3000:3000 \
  -e GF_INSTALL_PLUGINS=grafana-clickhouse-datasource \
  -e GF_AUTH_ANONYMOUS_ENABLED=true -e GF_AUTH_ANONYMOUS_ORG_ROLE=Viewer \
  -e GF_USERS_DEFAULT_THEME=dark -e GF_LOG_LEVEL=warn \
  -e CH_DM_PASSWORD -e CH_VICREAD_PASSWORD \
  -v "$PWD/grafana/provisioning:/etc/grafana/provisioning" \
  -v "$PWD/grafana/dashboards:/var/lib/grafana/dashboards" \
  grafana/grafana-oss:latest
# http://localhost:3000/d/fulltracetimetestnet/full-trace-time-testnet
# http://localhost:3000/d/fulltracetimemainnet/full-trace-time-mainnet
```

Anonymous viewer is on and the port binds to all interfaces — fine on a trusted LAN, not elsewhere.
Passwords come from the environment; the provisioning file holds no secrets.

Edit `make_local.py`, re-run it, and Grafana reloads within ~10s (`updateIntervalSeconds: 10`).

## Design notes — things that were got wrong once and should not be re-broken

**One x per message.** Series A carries the send time (`utime`), series B carries no timestamp at
all; the two are joined on the message hash **in Grafana** (`joinByField` on `msghash_hex`, mode
**inner**), so a message is plotted at exactly one x. Plotting the same message twice — at `utime`
and at `tx_now`, 0–1s apart — collapses Grafana's minimum x-gap, and bar width is *derived* from
that gap (`barWidthFactor` only multiplies it; there is no absolute bar-width setting). That is what
squashed every bar to a hairline. An earlier attempt worked around it with a 20s lattice plus
`WITH FILL`; that is gone, and should stay gone — see the fabrication note below.

**Use `inner`, not `outer`.** An outer join gives on-chain-only rows a missing time, which Grafana
fills with `0` — a bar at epoch 1970 and a permanent "Data outside time range" overlay.

**`WITH FILL` fabricates data.** It pads gaps with the column's **type default**, not NULL.
`finalized_action_in` is `Nullable(Float64)` so it filled with NULL correctly, but
`max(now) - min(now)` is a plain integer and filled with **0** — a zero-height bar at every
timestamp where no message exists. Measured: 45 non-null rows for 22 real messages. If any grid is
ever reintroduced, wrap the value in `toNullable(...)`.

**`join_use_nulls = 1`.** With ClickHouse's default, an unmatched `LEFT JOIN` row yields the type
default `0`, not NULL — so a streaming message with no on-chain trace would render as a genuine
"trace took 0 seconds". The 0 is produced by the join, before the aggregate, so a cast cannot
reach it. `variant_i` and the table panel carry the setting.

**A real `full_trace_time = 0` is legitimate** — on a quiet network the whole trace lands inside one
second. Only *filled* or *unmatched* slots may be NULL; genuine zeros must stay zero.

**Zoom-invariance.** The trace-duration subquery keeps the lower time bound (the external is the
trace's first tx, so the trace cannot start earlier) but **drops the upper bound**, so a tail
landing hours after the external is still counted; `trace_id IN (<anchor>)` keeps it cheap. Without
this, narrow windows truncated long traces and the same message reported different durations at
different zoom levels (measured 42s at 20 min vs 5303s unbounded).

**Join key is `hash_norm`, not the raw hash.** The streaming `msghash` is base64 and equals
`<net>_d.messages.hash_norm` (matched 83/83 on testnet, 180/180 on mainnet; 0 against the raw
hash). Both sides emit `lower(hex(base64Decode(...)))`.

**The streaming table is a `ReplacingMergeTree`** keyed on `(addr, utime)`, so a message can be
present as several not-yet-merged versions that disagree — one mainnet message had five rows, one
of them still with `finalized_action_in` NULL. They collapse on merge, but a query issued before
that must not show the stale version, and two rows for one message would also put two points at
one x (a 0s gap → hairlines). Hence `GROUP BY msghash` with `argMax(..., updated_at)`: the latest
version, which is what the merge would leave, and NULL-skipping so a finalized message is never
drawn as unfinalized. The **table** is deliberately not deduped: there a duplicate row is itself
worth seeing, and silently picking one of two disagreeing rows could show a message as
unfinalized when it finalized.

**Query limit is `LIMIT 60000`, and it truncates from the WRONG END.** The chart queries `ORDER BY ts` ascending, so hitting the limit drops the NEWEST rows, not the oldest — the panel silently shows an old slice of a wide window. At 2000 that was ~22 h of testnet probes (40 s cadence), which is why a week-long range only ever rendered one day. 60000 covers ~28 days on testnet and ~83 on mainnet. Measured cost of a 30-day window: 0.45 s for the streaming query, 1.3 s for the on-chain one.

**ClickHouse `formatDateTime('%M')` is the MONTH NAME**, not minutes.

**The zeros are real.** `full_trace_time = 0` dominates on an idle chain, and it is not missing
data: the indexer runs 1–3 s behind, the traces are complete (5 transactions, no unconsumed
out-messages), and all five land in the same second. `now` is the block's `gen_utime` in whole
seconds, so 1 s is the resolution floor and a sub-second trace can only read 0. Under load the
same traces stretched to thousands of seconds. What is *not* distinguished today is a trace still
in flight — its tail has not landed, so it reads short rather than unknown.

## Known state

- The live preview container may drift from these files until `make_local.py` is re-run against it.
