"""Builds the LOCAL preview dashboards, one per network.

Both networks use the identical panel design; only the databases, the monitor's sender wallet
and the datasource uids differ. The probe cadence differs too (testnet 40s, mainnet 120s) but
nothing in the design depends on it -- bar width follows from the real gap between messages,
which is exactly the point of joining on the message hash instead of quantizing time.
"""
import json
import os

MIXED = {"type": "datasource", "uid": "-- Mixed --"}

NETS = {
    "testnet": {
        "uid": "fulltracetimetestnet",
        "title": "Full trace time - testnet",
        "out": "grafana/dashboards/full_trace_testnet.json",
        "stream_ds": {"type": "grafana-clickhouse-datasource", "uid": "dmtestnetch"},
        "chain_ds": {"type": "grafana-clickhouse-datasource", "uid": "testnetdch"},
        "stream_tbl": "dm_testnet.toncenter_streaming_testnet",
        "chain_db": "testnet_d",
        # the streaming monitor's sender wallet, raw form
        "monitor": "0:CCA3DC4922DEAA32160983996E8482B5F179112552943DE3A0AC0D0E449A1381",
    },
    "mainnet": {
        "uid": "fulltracetimemainnet",
        "title": "Full trace time - mainnet",
        "out": "grafana/dashboards/full_trace_mainnet.json",
        "stream_ds": {"type": "grafana-clickhouse-datasource", "uid": "dmmainnetch"},
        "chain_ds": {"type": "grafana-clickhouse-datasource", "uid": "mainnetdch"},
        "stream_tbl": "dm_mainnet.toncenter_streaming_mainnet",
        "chain_db": "mainnet_d",
        # EQAIbKPNrlKWAvnJwTgmSB153yRzhRsKm06jNKv2H0tK6iQ1
        "monitor": "0:086CA3CDAE529602F9C9C13826481D79DF2473851B0A9B4EA334ABF61F4B4AEA",
    },
}

# BAR panel: A and B are two measurements of the SAME message, so they share ONE x.
# They are joined on the message identity (msghash) in Grafana, and the single x is series A's
# real, UNQUANTIZED send time. This removes the cause of the old hairline bars: previously the
# same message was plotted twice, at utime and at tx_now (0-1s apart), which collapsed Grafana's
# minimum-x-gap -- and bar width is derived from that gap (barWidthFactor only multiplies it;
# there is no absolute width setting). With one x per message the minimum gap is the real probe
# cadence, so bars sit on their true timestamps.
#
# GROUP BY msghash: the streaming table is a ReplacingMergeTree keyed on (addr, utime), so a
# message can be present as several not-yet-merged versions (seen 5x on mainnet) that disagree --
# one of those five still had finalized_action_in NULL while the others carried 0.733. They do
# collapse to a single row once ClickHouse merges, but a query issued before that must not show
# the stale version. Two rows for one message would also put two points at the same x, i.e. a 0s
# minimum gap and hairline bars again. argMax(..., updated_at) takes the value from the LATEST
# version, which is what the merge would eventually leave; it also skips NULLs, so a message that
# finalized never renders as if it had not. This is a dedupe of identical-identity rows, not an
# aggregation across messages.
A_BAR = """SELECT
  fromUnixTimestamp(toInt64(min(utime))) AS ts,
  lower(hex(base64Decode(msghash)))      AS msghash_hex,   /* join key only, hidden in the panel */
  argMax(finalized_action_in, updated_at) AS finalized_action_in
FROM {stream_tbl}
WHERE $__timeFilter(fromUnixTimestamp(toInt64(utime)))
GROUP BY msghash_hex
ORDER BY ts
LIMIT 60000"""

# B carries NO timestamp on purpose: the join attaches its value to A's row, so there is exactly
# one x per message. streaming.msghash (base64) == <chain_db>.messages.hash_norm (verified 180/180
# on mainnet, 83/83 on testnet; 0 against the raw hash). Both sides emit
# lower(hex(base64Decode(...))), the same key the table panel uses.
B_BAR = """SELECT
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
  /* ZOOM-INVARIANT: keep the LOWER bound (the external is the trace's first tx, so the
     trace cannot start earlier) but DROP the upper bound, so a tail landing hours after
     the external is still counted. trace_id IN (<same anchor>) keeps it cheap. */
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

# TABLE panel: joined by msghash so each row shows both numbers next to the hash.
# Deliberately NOT deduped: here a duplicate row is itself worth seeing, and silently picking one
# of two disagreeing rows could show a message as unfinalized when it finalized.
A_TBL = """SELECT
  fromUnixTimestamp(toInt64(utime))        AS ts,
  lower(hex(base64Decode(msghash)))        AS msghash_hex,   /* paste this into tonscan */
  msghash                                  AS msghash_base64,
  finalized_action_in
FROM {stream_tbl}
WHERE $__timeFilter(fromUnixTimestamp(toInt64(utime)))
ORDER BY ts
LIMIT 60000"""

B_TBL = """SELECT
  lower(hex(base64Decode(toString(m.hash_norm)))) AS msghash_hex,
  d.full_trace_time     AS full_trace_time
FROM
(
  SELECT hash_norm, tx_trace_id
  FROM {chain_db}.messages
  WHERE direction = 'in' AND source IS NULL
    AND destination = '{monitor}'
    AND tx_now >= toUnixTimestamp($__fromTime) - 300
    AND tx_now <= toUnixTimestamp($__toTime)
) AS m
INNER JOIN
(
  /* ZOOM-INVARIANT: lower bound only + anchor (see bar panel comment) */
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
LIMIT 60000"""

PANEL_DESC = ("full_trace_time = on-chain trace span (last tx block time - first tx block time, "
              "as in tonscan). finalized_action_in = when got finalized action from toncenter's "
              "streaming.")


def tgt(ds, sql, ref):
    return {"datasource": ds, "editorType": "sql", "queryType": "table",
            "rawSql": sql, "refId": ref, "format": 1}


bar_custom = {
    "drawStyle": "bars", "barAlignment": 0, "barWidthFactor": 0.8, "fillOpacity": 80,
    "lineWidth": 1, "gradientMode": "none", "pointSize": 5, "showPoints": "never",
    "spanNulls": False, "axisPlacement": "auto", "axisLabel": "seconds", "axisColorMode": "text",
    "scaleDistribution": {"type": "linear"}, "stacking": {"mode": "none", "group": "A"},
    "hideFrom": {"legend": False, "tooltip": False, "viz": False},
    "thresholdsStyle": {"mode": "off"},
}

overrides = [
    {"matcher": {"id": "byName", "options": "full_trace_time"},
     "properties": [{"id": "color", "value": {"mode": "fixed", "fixedColor": "blue"}},
                    {"id": "displayName", "value": "full_trace_time (on-chain)"}]},
    {"matcher": {"id": "byName", "options": "finalized_action_in"},
     "properties": [{"id": "color", "value": {"mode": "fixed", "fixedColor": "orange"}},
                    {"id": "displayName", "value": "finalized_action_in (streaming)"}]},
]


def build(net):
    sql = lambda t: t.format(**net)
    bar_panel = {
        "id": 100, "type": "timeseries", "title": "Jetton Transfer time",
        "description": PANEL_DESC,
        "datasource": MIXED,
        "gridPos": {"h": 11, "w": 24, "x": 0, "y": 0},
        "pluginVersion": "12.1.1",
        "fieldConfig": {"defaults": {"unit": "s", "color": {"mode": "palette-classic"},
                                     "custom": bar_custom,
                                     "thresholds": {"mode": "absolute",
                                                    "steps": [{"color": "green", "value": None}]}},
                        "overrides": overrides},
        "options": {"legend": {"displayMode": "list", "placement": "bottom", "showLegend": True,
                               "calcs": []},
                    "tooltip": {"mode": "multi", "sort": "desc"}},
        "targets": [tgt(net["stream_ds"], sql(A_BAR), "A"), tgt(net["chain_ds"], sql(B_BAR), "B")],
        "transformations": [
            # INNER, not outer: outer keeps B-only rows (an on-chain message the monitor never
            # logged), which have no ts -> the join filled time with 0 -> a bar at epoch 1970 and
            # the permanent "Data outside time range" overlay. Inner drops those.
            {"id": "joinByField", "options": {"byField": "msghash_hex", "mode": "inner"}},
            # the hash is only the join key -- keep it out of the plot
            {"id": "organize", "options": {"excludeByName": {"msghash_hex": True},
                                           "renameByName": {}, "indexByName": {}}},
        ],
    }

    table_panel = {
        "id": 101, "type": "table", "title": "TX hashes + full_trace_time (merged)",
        "datasource": MIXED,
        "gridPos": {"h": 12, "w": 24, "x": 0, "y": 11},
        "pluginVersion": "12.1.1",
        "fieldConfig": {"defaults": {"custom": {"align": "auto",
                                                "cellOptions": {"type": "auto"}, "inspect": False},
                                     "thresholds": {"mode": "absolute",
                                                    "steps": [{"color": "green", "value": None}]}},
                        "overrides": [
                            {"matcher": {"id": "byName", "options": "full_trace_time"},
                             "properties": [{"id": "unit", "value": "s"}]},
                            {"matcher": {"id": "byName", "options": "finalized_action_in"},
                             "properties": [{"id": "unit", "value": "s"}]},
                        ]},
        "options": {"showHeader": True, "cellHeight": "sm",
                    "sortBy": [{"displayName": "ts", "desc": True}],
                    "footer": {"show": False, "reducer": ["sum"], "countRows": False,
                               "fields": ""}},
        "targets": [tgt(net["stream_ds"], sql(A_TBL), "A"), tgt(net["chain_ds"], sql(B_TBL), "B")],
        "transformations": [{"id": "joinByField",
                             "options": {"byField": "msghash_hex", "mode": "outer"}}],
    }

    return {
        "annotations": {"list": []},
        "editable": True,
        "fiscalYearStartMonth": 0,
        "graphTooltip": 0,
        "id": None,
        "links": [],
        "panels": [bar_panel, table_panel],
        "refresh": "",
        "schemaVersion": 39,
        "tags": [],
        "templating": {"list": []},
        "time": {"from": "now-3h", "to": "now"},
        "timepicker": {},
        "timezone": "browser",
        "title": net["title"],
        "uid": net["uid"],
        "version": 1,
        "weekStart": "",
    }


here = os.path.dirname(os.path.abspath(__file__))
outdir = os.environ.get("OUTDIR", here)
for name, net in NETS.items():
    path = os.path.join(outdir, net["out"])
    json.dump(build(net), open(path, "w"), indent=2)
    print("wrote", path)
