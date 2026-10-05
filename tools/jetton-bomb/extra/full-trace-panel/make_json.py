"""Builds the HAND-OFF json for toncenter's dashboard admin, one file per network.

Output: full_trace_panel_testnet.json / full_trace_panel_mainnet.json.
Not meant to be pasted whole into Grafana -- see the _instructions key in each file.
"""
import json
import os

DS_MIXED = {"type": "datasource", "uid": "-- Mixed --"}

NETS = {
    "testnet": {
        "net": "testnet",
        "out": "full_trace_panel_testnet.json",
        "dash": "toncenter 'Analytics' dashboard (uid af5z8mnmm15hcb)",
        "stream_tbl": "dm_testnet.toncenter_streaming_testnet",
        "chain_db": "testnet_d",
        "monitor": "0:CCA3DC4922DEAA32160983996E8482B5F179112552943DE3A0AC0D0E449A1381",
        "cadence": "40 s",
        # local preview datasource uids -- the admin swaps these for prod ones
        "dm_uid": "dmtestnetch",
        "chain_uid": "testnetdch",
        "prod_uid": "ff5z8xz8e1urke",
        "hash_evidence": "83/83",
    },
    "mainnet": {
        "net": "mainnet",
        "out": "full_trace_panel_mainnet.json",
        "dash": "the mainnet Analytics dashboard (ask the admin for its uid)",
        "stream_tbl": "dm_mainnet.toncenter_streaming_mainnet",
        "chain_db": "mainnet_d",
        # EQAIbKPNrlKWAvnJwTgmSB153yRzhRsKm06jNKv2H0tK6iQ1
        "monitor": "0:086CA3CDAE529602F9C9C13826481D79DF2473851B0A9B4EA334ABF61F4B4AEA",
        "cadence": "120 s",
        "dm_uid": "dmmainnetch",
        "chain_uid": "mainnetdch",
        "prod_uid": "ff5z8xz8e1urke",
        "hash_evidence": "180/180",
    },
}

# ---------------------------------------------------------------------------
# JOIN: streaming.msghash is BASE64 and equals <chain_db>.messages.hash_norm (the NORMALIZED
# external hash). Both sides are emitted as lower(hex(base64Decode(...))) -- the 64-char form
# tonscan takes. Matching against the RAW hash returns nothing.
# full_trace_time = max(now) - min(now) over the trace's transactions. There is no traces table.
# ---------------------------------------------------------------------------

# LEFT-join block for the TABLE panel: emits both hash forms as keys, so it matches whichever the
# streaming row carries.
MAP_BOTH = """  LEFT JOIN
  (
    SELECT
      arrayJoin([toString(m.hash), toString(m.hash_norm)]) AS msghash,
      d.full_trace_time AS full_trace_time
    FROM
    (
      SELECT hash, hash_norm, tx_trace_id
      FROM {chain_db}.messages
      WHERE direction = 'in' AND source IS NULL
        AND destination = '{monitor}'  /* monitor sender wallet */
        AND tx_now >= toUnixTimestamp($__fromTime) - 300
        AND tx_now <= toUnixTimestamp($__toTime)
    ) AS m
    INNER JOIN
    (
      /* ZOOM-INVARIANT: keep the LOWER bound (the external is the trace's first tx, so the
         trace cannot start earlier) but DROP the upper bound, so a tail landing hours after
         the external is still fully counted. Without this, max(now)-min(now) is truncated by
         the panel window and the SAME msghash changes value when you zoom.
         trace_id IN (<same anchor>) keeps the unbounded scan cheap. */
      SELECT trace_id, max(now) - min(now) AS full_trace_time
      FROM {chain_db}.transactions
      WHERE now >= toUnixTimestamp($__fromTime) - 300
        AND trace_id IN (
          SELECT tx_trace_id FROM {chain_db}.messages
          WHERE direction = 'in' AND source IS NULL
            AND destination = '{monitor}'
            AND tx_now >= toUnixTimestamp($__fromTime) - 300
            AND tx_now <= toUnixTimestamp($__toTime)
        )
      GROUP BY trace_id
    ) AS d ON m.tx_trace_id = d.trace_id
  ) AS tr ON s.msghash = tr.msghash"""

# INNER-join block for variant i's chart, so it shows the same set as variant ii.
MAP_CHART = """  INNER JOIN
  (
    SELECT
      lower(hex(base64Decode(toString(m.hash_norm)))) AS msghash_hex,
      d.full_trace_time AS full_trace_time
    FROM
    (
      SELECT hash_norm, tx_trace_id
      FROM {chain_db}.messages
      WHERE direction = 'in' AND source IS NULL
        AND destination = '{monitor}'  /* monitor sender wallet */
        AND tx_now >= toUnixTimestamp($__fromTime)
        AND tx_now <= toUnixTimestamp($__toTime)
    ) AS m
    INNER JOIN
    (
      /* ZOOM-INVARIANT: lower bound kept, upper bound DROPPED, trace_id IN (<anchor>) */
      SELECT trace_id, max(now) - min(now) AS full_trace_time
      FROM {chain_db}.transactions
      WHERE now >= toUnixTimestamp($__fromTime) - 300
        AND trace_id IN (
          SELECT tx_trace_id FROM {chain_db}.messages
          WHERE direction = 'in' AND source IS NULL
            AND destination = '{monitor}'
            AND tx_now >= toUnixTimestamp($__fromTime) - 300
            AND tx_now <= toUnixTimestamp($__toTime)
        )
      GROUP BY trace_id
    ) AS d ON m.tx_trace_id = d.trace_id
  ) AS tr ON lower(hex(base64Decode(s.msghash))) = tr.msghash_hex"""

VI_BAR_SQL = """SELECT
  /* ONE x per message: the real, UNQUANTIZED send time. The on-chain value is attached by joining
     on the message identity, so a message is never plotted at two different x values -- that
     double-plotting is what collapsed Grafana's minimum x-gap (bar width is derived from it) and
     squashed every bar to a hairline. Nothing is quantized, filled or smoothed. */
  fromUnixTimestamp(toInt64(min(s.utime)))       AS ts,
  argMax(s.finalized_action_in, s.updated_at)    AS finalized_action_in,
  max(tr.full_trace_time)                        AS full_trace_time
FROM {stream_tbl} AS s
{MAP_CHART}
WHERE $__timeFilter(fromUnixTimestamp(toInt64(s.utime)))
/* GROUP BY the message: the streaming table is a ReplacingMergeTree keyed on (addr, utime), so a
   message can appear as several not-yet-merged versions that disagree -- one mainnet message had
   five rows, one of them still with finalized_action_in NULL. argMax(..., updated_at) takes the
   latest version, i.e. what the merge would eventually leave, and skips NULLs. */
GROUP BY lower(hex(base64Decode(s.msghash)))
ORDER BY ts
LIMIT 60000
SETTINGS join_use_nulls = 1   /* unmatched join row -> NULL, not a fabricated 0 */"""

TABLE_SQL = """SELECT
  fromUnixTimestamp(toInt64(s.utime)) AS ts,
  lower(hex(base64Decode(s.msghash))) AS msghash_hex,   /* paste this into tonscan */
  s.msghash                           AS msghash_base64,
  tr.full_trace_time                  AS full_trace_time
  /* keep your existing panel-6 columns here, e.g. s.finalized_action_in, s.finalized_tx_in, ... */
FROM {stream_tbl} AS s
{MAP_BOTH}
WHERE $__timeFilter(fromUnixTimestamp(toInt64(s.utime)))
ORDER BY ts DESC
LIMIT 60000
SETTINGS join_use_nulls = 1   /* unmatched LEFT JOIN -> NULL, not a fabricated 0 */"""

VII_A_SQL = """SELECT
  /* ONE x per message: the real, UNQUANTIZED send time. Series B carries NO timestamp -- the
     Grafana join on msghash attaches its value to this row, so a message has exactly one x. */
  fromUnixTimestamp(toInt64(min(utime)))  AS ts,
  lower(hex(base64Decode(msghash)))       AS msghash_hex,   /* join key only, hidden in the panel */
  argMax(finalized_action_in, updated_at) AS finalized_action_in
FROM {stream_tbl}
WHERE $__timeFilter(fromUnixTimestamp(toInt64(utime)))
/* see the ReplacingMergeTree note in variant i */
GROUP BY msghash_hex
ORDER BY ts
LIMIT 60000"""

VII_B_SQL = """SELECT
  /* streaming.msghash (base64) == {chain_db}.messages.hash_norm; both sides emit
     lower(hex(base64Decode(...))), the same key the table panel uses. No timestamp here. */
  lower(hex(base64Decode(toString(m.hash_norm)))) AS msghash_hex,
  max(d.full_trace_time)                          AS full_trace_time
FROM
(
  SELECT hash_norm, tx_trace_id
  FROM {chain_db}.messages
  WHERE direction = 'in' AND source IS NULL
    AND destination = '{monitor}'  /* monitor sender wallet */
    AND tx_now >= toUnixTimestamp($__fromTime)
    AND tx_now <= toUnixTimestamp($__toTime)
) AS m
INNER JOIN
(
  /* ZOOM-INVARIANT: keep the LOWER bound (the external is the trace's first tx, so the trace
     cannot start earlier) but DROP the upper bound so a tail landing hours later is counted.
     trace_id IN (<same anchor>) keeps it cheap. */
  SELECT trace_id, max(now) - min(now) AS full_trace_time
  FROM {chain_db}.transactions
  WHERE now >= toUnixTimestamp($__fromTime) - 300
    AND trace_id IN (
      SELECT tx_trace_id FROM {chain_db}.messages
      WHERE direction = 'in' AND source IS NULL
        AND destination = '{monitor}'
        AND tx_now >= toUnixTimestamp($__fromTime) - 300
        AND tx_now <= toUnixTimestamp($__toTime)
    )
  GROUP BY trace_id
) AS d ON m.tx_trace_id = d.trace_id
GROUP BY msghash_hex
LIMIT 60000"""

PANEL_DESC = ("full_trace_time = on-chain trace span (last tx block time - first tx block time, "
              "as in tonscan). finalized_action_in = when got finalized action from toncenter's "
              "streaming.")


def sql_target(ds, sql, refId):
    return {"datasource": ds, "editorType": "sql", "queryType": "table",
            "rawSql": sql, "refId": refId, "format": 1}


def bar_fieldconfig():
    return {
        "defaults": {
            "unit": "s",
            "color": {"mode": "palette-classic"},
            "custom": {
                "drawStyle": "bars", "barAlignment": 0, "barWidthFactor": 0.6,
                "fillOpacity": 80, "lineWidth": 1, "gradientMode": "none",
                "pointSize": 5, "showPoints": "never", "spanNulls": False,
                "axisPlacement": "auto", "axisLabel": "seconds", "axisColorMode": "text",
                "scaleDistribution": {"type": "linear"},
                "stacking": {"mode": "none", "group": "A"},
                "hideFrom": {"legend": False, "tooltip": False, "viz": False},
                "thresholdsStyle": {"mode": "off"},
            },
            "thresholds": {"mode": "absolute", "steps": [{"color": "green", "value": None}]},
        },
        "overrides": [
            {"matcher": {"id": "byName", "options": "full_trace_time"},
             "properties": [{"id": "color", "value": {"mode": "fixed", "fixedColor": "blue"}},
                            {"id": "displayName", "value": "full_trace_time (on-chain)"}]},
            {"matcher": {"id": "byName", "options": "finalized_action_in"},
             "properties": [{"id": "color", "value": {"mode": "fixed", "fixedColor": "orange"}},
                            {"id": "displayName", "value": "finalized_action_in (streaming)"}]},
        ],
    }


bar_options = {
    "legend": {"displayMode": "list", "placement": "bottom", "showLegend": True, "calcs": []},
    "tooltip": {"mode": "multi", "sort": "desc"},
}


def build(n):
    ds_prod = {"type": "grafana-clickhouse-datasource", "uid": n["prod_uid"]}
    ds_dm = {"type": "grafana-clickhouse-datasource", "uid": n["dm_uid"]}
    ds_chain = {"type": "grafana-clickhouse-datasource", "uid": n["chain_uid"]}

    ctx = dict(n)
    ctx["MAP_CHART"] = MAP_CHART.format(**n)
    ctx["MAP_BOTH"] = MAP_BOTH.format(**n)

    variant_i = {
        "id": 100, "type": "timeseries", "title": "Jetton Transfer time",
        "description": PANEL_DESC + " [variant i: single query; datasource CH user must have "
                                    "grants on BOTH %s and %s]" % (n["stream_tbl"].split(".")[0],
                                                                   n["chain_db"]),
        "datasource": ds_prod,
        "gridPos": {"h": 9, "w": 24, "x": 0, "y": 40},
        "pluginVersion": "12.1.1",
        "fieldConfig": bar_fieldconfig(), "options": bar_options,
        "targets": [sql_target(ds_prod, VI_BAR_SQL.format(**ctx), "A")],
    }

    variant_ii = {
        "id": 100, "type": "timeseries", "title": "Jetton Transfer time",
        "description": PANEL_DESC + " [variant ii: A=%s streaming, B=%s on-chain, joined in "
                                    "Grafana on the message hash]" % (n["stream_tbl"].split(".")[0],
                                                                      n["chain_db"]),
        "datasource": DS_MIXED,
        "gridPos": {"h": 9, "w": 24, "x": 0, "y": 40},
        "pluginVersion": "12.1.1",
        "fieldConfig": bar_fieldconfig(), "options": bar_options,
        "targets": [sql_target(ds_dm, VII_A_SQL.format(**ctx), "A"),
                    sql_target(ds_chain, VII_B_SQL.format(**ctx), "B")],
        "transformations": [
            # INNER, not outer: outer keeps B-only rows (an on-chain message the monitor never
            # logged); those have no ts, the join fills time with 0, and Grafana draws a bar at
            # epoch 1970 -> a permanent "Data outside time range" overlay.
            {"id": "joinByField", "options": {"byField": "msghash_hex", "mode": "inner"}},
            # the hash is only the join key -- keep it out of the plot
            {"id": "organize", "options": {"excludeByName": {"msghash_hex": True},
                                           "renameByName": {}, "indexByName": {}}},
        ],
    }

    table_panel = {
        "id": 6, "type": "table", "title": "TX hashes",
        "description": "REFERENCE ONLY - merge the full_trace_time column + the LEFT JOIN block "
                       "into your live panel 6; keep its other columns/options. Uses the "
                       "single-query cross-DB form (variant i); if the datasource cannot read "
                       "both DBs, build the table the same two-datasource way as variant ii.",
        "datasource": ds_prod,
        "gridPos": {"h": 9, "w": 24, "x": 0, "y": 31},
        "pluginVersion": "12.1.1",
        "fieldConfig": {
            "defaults": {"custom": {"align": "auto", "cellOptions": {"type": "auto"},
                                    "inspect": False},
                         "thresholds": {"mode": "absolute",
                                        "steps": [{"color": "green", "value": None}]}},
            "overrides": [{"matcher": {"id": "byName", "options": "full_trace_time"},
                           "properties": [{"id": "unit", "value": "s"}]}],
        },
        "options": {"showHeader": True, "cellHeight": "sm",
                    "footer": {"show": False, "reducer": ["sum"], "countRows": False,
                               "fields": ""}},
        "targets": [sql_target(ds_prod, TABLE_SQL.format(**ctx), "A")],
    }

    stream_db = n["stream_tbl"].split(".")[0]
    instructions = [
        "NETWORK: %s. Target: %s. Do NOT paste this whole file into Grafana." % (n["net"], n["dash"]),

        "JOIN (verified against real data): streaming.msghash is BASE64 and equals "
        "%s.messages.hash_norm -- the NORMALIZED external hash. Both sides are emitted as "
        "lower(hex(base64Decode(...))), the 64-char form tonscan takes. Matched %s on hash_norm "
        "and 0 on the raw hash. full_trace_time = max(now)-min(now) over %s.transactions grouped "
        "by trace_id; there is NO traces table on this cluster."
        % (n["chain_db"], n["hash_evidence"], n["chain_db"]),

        "PICK A VARIANT BY THE DATASOURCE'S CH GRANTS -- that is the only difference. The panel "
        "chrome is byte-identical between them (type timeseries, drawStyle bars, unit s, the two "
        "colour/displayName overrides). variant_i_single_query_cross_db = ONE target on ONE "
        "datasource whose CH user must read BOTH %s and %s, doing the join in SQL. "
        "variant_ii_two_datasource_DEPLOYABLE = TWO targets on two single-DB datasources under "
        "'-- Mixed --', joined in Grafana. On our cluster no single user reads both DBs (dm reads "
        "%s only, vicread reads %s only), so variant i could not be run end to end here."
        % (stream_db, n["chain_db"], stream_db, n["chain_db"]),

        "ONE X PER MESSAGE -- this is the load-bearing idea, do not undo it. Series A and B are "
        "two measurements of the SAME message, so they must share one x. A carries the send time "
        "(utime); B carries NO timestamp and is attached by joining on the message hash. Plotting "
        "the same message twice -- at utime and at the on-chain tx_now, 0-1s apart -- collapses "
        "Grafana's MINIMUM x-gap, and bar width is DERIVED from that gap (barWidthFactor only "
        "multiplies it; there is no absolute width setting). That is what rendered every bar as a "
        "hairline, and it flipped bars thin/thick between nearly identical windows depending on "
        "whether such a 1s pair fell inside. With one x per message the minimum gap is the real "
        "probe cadence (%s here), so bars sit on their true timestamps at a stable width."
        % n["cadence"],

        "REJECTED DEAD ENDS, do not reintroduce: (1) quantizing ts onto a 20s lattice with "
        "toStartOfInterval + ORDER BY ts WITH FILL. It did make the width deterministic, but "
        "WITH FILL pads gaps with the column's TYPE DEFAULT, not NULL -- and max(now)-min(now) is "
        "a plain integer, so every empty slot became a real-looking full_trace_time of 0, i.e. a "
        "zero-height bar at timestamps where no message exists (measured 45 non-null rows for 22 "
        "real messages). (2) Making ts a STRING (toString/substring/concat), with or without the "
        "msghash appended as a label. Any string ts forces the panel to `barchart`, whose x is "
        "CATEGORICAL, so Grafana prints a label under EVERY bar -- at -45 degrees that is an "
        "unreadable smear filling half the panel. It also renders the label in the ClickHouse "
        "session timezone, so the axis disagreed with the time picker by 3 hours. The hash belongs "
        "ONLY in the table panel.",

        "REPLACINGMERGETREE: the streaming table is keyed on (addr, utime), so a message can be "
        "present as several not-yet-merged versions that disagree -- one mainnet message had five "
        "rows, one of them still with finalized_action_in NULL while the others carried 0.733. "
        "They collapse once ClickHouse merges, but a query issued before that must not show the "
        "stale version, and two rows for one message would also put two points at one x (a 0s gap "
        "-> hairlines). Hence GROUP BY the message hash with argMax(finalized_action_in, "
        "updated_at): the latest version, which is what the merge would leave, and it skips NULLs "
        "so a finalized message is never drawn as unfinalized.",

        "join_use_nulls = 1 IS REQUIRED. With ClickHouse's default an unmatched LEFT JOIN row "
        "yields the type default 0, not NULL -- so a streaming message with no on-chain trace "
        "would render as a genuine 'trace took 0 seconds'. The 0 is produced by the join, before "
        "the aggregate, so a cast cannot reach it.",

        "ZOOM-INVARIANCE -- do NOT reintroduce an upper time bound on the transactions scan. "
        "full_trace_time must depend only on the trace, never on the panel window. Bounding by "
        "'now <= $__toTime' truncates traces whose last hop lands after the window (under "
        "congestion the tail arrived over an hour after the external), so the SAME msghash read "
        "42s zoomed in and 5303s zoomed out. Fix: anchor the trace set on monitor externals inside "
        "the window, KEEP the lower bound (the external is the trace's first tx, so the trace "
        "cannot start earlier), DROP the upper bound, and add trace_id IN (<same anchor>) to keep "
        "the unbounded scan cheap. Verified identical at 3h vs 20min windows.",

        "OVERLAY: use joinByField mode 'inner'. An outer join keeps on-chain-only rows (a msghash "
        "the monitor never logged), fills their ts with 0, and Grafana plots that at 1970 -- a "
        "permanent 'Data outside time range' overlay that cannot be filtered away (the join fills "
        "0, not SQL NULL). Inner drops those rows; in a 3h window that was 3 of 265 on testnet "
        "and 0 of 90 on mainnet, all at the window edges.",

        "MONITOR WALLET: '%s'. The monitor's externals have no fixed opcode, so the destination is "
        "the stable filter. If the monitor rotates wallets, refresh it: take recent streaming "
        "msghashes and run  SELECT destination, count() FROM %s.messages WHERE "
        "lower(hex(base64Decode(toString(hash_norm)))) IN (<hashes>) AND direction='in' AND "
        "source IS NULL GROUP BY destination ORDER BY 2 DESC."
        % (n["monitor"], n["chain_db"]),

        "SWAP DATASOURCE UIDS: variant ii uses the local preview uids '%s' (%s) and '%s' (%s). "
        "Replace with your prod uids -- one CH datasource per DB/user. variant i uses %s."
        % (n["dm_uid"], stream_db, n["chain_uid"], n["chain_db"], n["prod_uid"]),

        "UNITS: finalized_action_in is Nullable(Float64) SECONDS; panel unit 's' is correct, no "
        "/1000.",

        "READING IT: the point is the contrast. During the 31.08 testnet load run most probes had "
        "NULL finalized_action_in (13.6% present in the last hour, 0% in the last 30 minutes) "
        "while their on-chain traces had long completed -- streaming lagged 8-11 minutes behind a "
        "chain that was keeping up.",

        "ZEROS ARE REAL, not missing data. On an idle chain full_trace_time is 0 almost "
        "everywhere: the indexer runs 1-3s behind, the traces are complete (5 transactions, no "
        "unconsumed out-messages), and all five land in the same second. `now` is the block's "
        "gen_utime in WHOLE SECONDS, so 1s is the resolution floor and a sub-second trace can "
        "only read 0. Under load the same traces stretched to thousands of seconds.",

        "IN-FLIGHT CAVEAT: a trace whose tail has not landed when the query runs reads SHORT and "
        "grows on a later refresh -- it is measuring an unfinished trace, not a fast one. No query "
        "change can fix it (the tail does not exist yet); distinguishing it would need a "
        "completeness check, e.g. requiring every out-message of the trace to have a matching in-tx.",

        "PERF (measured on testnet under ~5M tx/h): the zoom-invariant on-chain query ran 94ms "
        "over a 3h window and 74ms over 20 minutes; the streaming query ~6ms. Dropping the upper "
        "bound did NOT make it slower -- the trace_id IN anchor prunes the scan.",

        "TABLE (panel 6): add tr.full_trace_time to its SELECT and splice the LEFT JOIN block "
        "(table_column_addition__left_join_block) before its WHERE; alias streaming as s and "
        "prefix its columns with s. Do NOT dedupe the table the way the chart does -- there a "
        "duplicate row is itself worth seeing, and silently picking one of two disagreeing rows "
        "could show a message as unfinalized when it finalized.",
    ]

    return {
        "_instructions": instructions,
        "variant_i_single_query_cross_db": variant_i,
        "variant_ii_two_datasource_DEPLOYABLE": variant_ii,
        "modified_table_panel_id6": table_panel,
        "table_column_addition__left_join_block": MAP_BOTH.format(**n),
    }


here = os.path.dirname(os.path.abspath(__file__))
outdir = os.environ.get("OUTDIR", here)
for name, net in NETS.items():
    path = os.path.join(outdir, net["out"])
    json.dump(build(net), open(path, "w"), indent=2)
    print("wrote", path)
