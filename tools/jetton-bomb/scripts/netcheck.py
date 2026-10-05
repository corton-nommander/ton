#!/usr/bin/env python3
"""netcheck — снимок сети перед/после нагрузочного теста (testnet / mainnet).

Зачем: перед каждым прогоном нужно одно и то же — версии/конфиг сети, max_split,
лимиты блока, proto_version (старые/новые бродкасты), число валидаторов и
коллаторов, органический tps и блокрейт; после прогона — max 15-min avg tps,
блокрейт и число шардов за окно. Всё это здесь, чтобы не разбирать BOC-и и не
вспоминать SQL заново. Подробности — в docs/LOADTEST_RUNBOOK.md.

Использование:
  python3 scripts/netcheck.py config  [testnet|mainnet]     параметры конфига + ссылка на эксплорер
  python3 scripts/netcheck.py diff                          testnet vs mainnet по ключевым параметрам
  python3 scripts/netcheck.py status [testnet|mainnet]      шарды / органика / блокрейт сейчас (ClickHouse)
  python3 scripts/netcheck.py run <from> <to> [net]         метрики прогона за окно UTC
                                                            ('2026-08-24 16:00' или '2026-08-24T16:00:00')

ClickHouse: креды из окружения CH_USER/CH_PASS (или configs-private/loadtest.env).
"""

import base64
import json
import os
import struct
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone, timedelta

API = {
    "testnet": "https://testnet.toncenter.com/api/v2",
    "mainnet": "https://toncenter.com/api/v2",
}
EXPLORER = {
    "testnet": "https://test-explorer.toncoin.org",
    "mainnet": "https://explorer.toncoin.org",
}
CH_DB = {"testnet": "testnet_d", "mainnet": "mainnet_d"}
CH_URL = os.environ.get("CH_URL", "http://tc-ch-02.toncenter.local:8123/")

# --------------------------------------------------------------------------- BOC


def parse_boc(raw: bytes):
    """Минимальный BOC-парсер: возвращает список ячеек (bits:str, refs:list[int])."""
    assert raw[:4].hex() == "b5ee9c72", f"не BOC: {raw[:4].hex()}"
    i = 4
    flags = raw[i]
    i += 1
    has_idx = bool(flags & 0x80)
    ref_sz = flags & 7
    off_sz = raw[i]
    i += 1

    def rd(n):
        nonlocal i
        v = int.from_bytes(raw[i : i + n], "big")
        i += n
        return v

    cells, roots, _absent, _tot = rd(ref_sz), rd(ref_sz), rd(ref_sz), rd(off_sz)
    root_list = [rd(ref_sz) for _ in range(roots)]
    if has_idx:
        i += cells * off_sz
    out = []
    for _ in range(cells):
        d1, d2 = raw[i], raw[i + 1]
        i += 2
        nrefs = d1 & 7
        data = raw[i : i + (d2 + 1) // 2]
        i += (d2 + 1) // 2
        refs = [int.from_bytes(raw[i + k * ref_sz : i + (k + 1) * ref_sz], "big") for k in range(nrefs)]
        i += nrefs * ref_sz
        bits = "".join(f"{b:08b}" for b in data)
        if d2 % 2:  # нечётный d2 => последний байт с completion-тегом
            bits = bits[: bits.rfind("1")]
        else:
            bits = bits[: d2 * 4]
        out.append((bits, refs))
    return out, root_list


class Slice:
    def __init__(self, cells, idx):
        self.cells = cells
        self.bits, self.refs = cells[idx]
        self.p = 0

    def u(self, n):
        v = int(self.bits[self.p : self.p + n], 2)
        self.p += n
        return v

    def i(self, n):
        v = self.u(n)
        return v - (1 << n) if v >> (n - 1) else v

    def bit(self):
        return self.u(1)

    def rest(self):
        return len(self.bits) - self.p

    def ref(self, k):
        return Slice(self.cells, self.refs[k])


def hashmap(sl: Slice, key_bits: int, prefix="") -> dict:
    """HashmapE n X -> {key:int -> Slice, спозиционированный на значение}."""
    # label
    if sl.bit() == 0:  # hml_short$0 unary_len
        n = 0
        while sl.bit() == 1:
            n += 1
        label = "".join(str(sl.bit()) for _ in range(n))
    elif sl.bit() == 0:  # hml_long$10
        n = sl.u(max(1, (key_bits - len(prefix)).bit_length()))
        label = "".join(str(sl.bit()) for _ in range(n))
    else:  # hml_same$11
        v = sl.bit()
        n = sl.u(max(1, (key_bits - len(prefix)).bit_length()))
        label = str(v) * n
    prefix += label
    if len(prefix) == key_bits:
        return {int(prefix, 2) if prefix else 0: sl}
    out = {}
    for b in (0, 1):
        out.update(hashmap(sl.ref(b), key_bits, prefix + str(b)))
    return out


def http_json(url: str, tries: int = 6):
    """toncenter без ключа отдаёт 429 при частых запросах — ретраим с паузой.
    Ключ (если есть): export TONCENTER_API_KEY=... / TONCENTER_TESTNET_API_KEY=..."""
    key = os.environ.get("TONCENTER_API_KEY") if "testnet" not in url else os.environ.get(
        "TONCENTER_TESTNET_API_KEY", os.environ.get("TONCENTER_API_KEY", "")
    )
    req = urllib.request.Request(url, headers={"X-API-Key": key} if key else {})
    for attempt in range(tries):
        try:
            return json.load(urllib.request.urlopen(req, timeout=25))
        except urllib.error.HTTPError as e:
            if e.code not in (429, 500, 502, 503) or attempt == tries - 1:
                raise
            time.sleep(1.5 * (attempt + 1))


def fetch_param(net: str, pid: int) -> bytes:
    d = http_json(f"{API[net]}/getConfigParam?config_id={pid}")
    return base64.b64decode(d["result"]["config"]["bytes"])


def root_slice(net: str, pid: int) -> Slice:
    cells, roots = parse_boc(fetch_param(net, pid))
    return Slice(cells, roots[0])


# ------------------------------------------------------------------- декодеры


def p8(net):
    s = root_slice(net, 8)
    assert s.u(8) == 0xC4
    return {"version": s.u(32), "capabilities": s.u(64)}


def p12(net):
    s = root_slice(net, 12)
    out = {}
    if not s.bit():  # hme_empty$0
        return out
    for wc, v in hashmap(s.ref(0), 32).items():
        tag = v.u(8)
        r = {
            "tag": hex(tag),
            "enabled_since": v.u(32),
            "monitor_min_split": v.u(8),
            "min_split": v.u(8),
            "max_split": v.u(8),
        }
        v.u(1 + 1 + 1 + 13)  # basic / active / accept_msgs / flags
        # Хвост читаем С КОНЦА: середина (format) уезжает на пару бит между версиями.
        if tag == 0xA7 and len(v.bits) >= 136:
            tail = v.bits[-136:]
            r["split_merge_timings"] = [int(tail[k * 32 : (k + 1) * 32], 2) for k in range(4)]
            r["persistent_state_split_depth"] = int(v.bits[-8:], 2)
        out[wc if wc < (1 << 31) else wc - (1 << 32)] = r
    return out


def limits(net, pid):
    s = root_slice(net, pid)
    assert s.u(8) == 0x5D
    res = {}
    for name in ("bytes", "gas", "lt_delta"):
        assert s.u(8) == 0xC3
        res[name] = [s.u(32), s.u(32), s.u(32)]  # underload / soft / hard
    return res


def p29(net):
    s = root_slice(net, 29)
    tag = s.u(8)
    if tag != 0xD9:  # только consensus_config_v4 разбираем детально
        return {"tag": hex(tag)}
    s.u(6)
    return {
        "use_quic": s.bit(),
        "new_catchain_ids": s.bit(),
        "round_candidates": s.u(8),
        "next_candidate_delay_ms": s.u(32),
        "consensus_timeout_ms": s.u(32),
        "fast_attempts": s.u(32),
        "attempt_duration": s.u(32),
        "catchain_max_deps": s.u(32),
        "max_block_bytes": s.u(32),
        "max_collated_bytes": s.u(32),
        "proto_version": s.u(16),
        "catchain_max_blocks_coeff": s.u(32),
    }


NONCRITICAL = {  # id -> (имя, тип)  из ton/ton-types.h ENUMERATE_NONCRITICAL_PARAMS
    0: ("target_rate_ms", "u"),
    1: ("first_block_timeout_ms", "u"),
    2: ("first_block_timeout_multiplier", "f"),
    3: ("first_block_timeout_cap_ms", "u"),
    4: ("candidate_resolve_timeout_ms", "u"),
    5: ("candidate_resolve_timeout_multiplier", "f"),
    6: ("candidate_resolve_timeout_cap_ms", "u"),
    7: ("candidate_resolve_cooldown_ms", "u"),
    8: ("standstill_timeout_ms", "u"),
    9: ("standstill_max_egress_bytes_per_s", "u"),
    10: ("max_leader_window_desync", "u"),
    11: ("bad_signature_ban_duration_ms", "u"),
    12: ("candidate_resolve_rate_limit", "u"),
    13: ("min_block_interval_ms", "u"),
    14: ("no_empty_blocks_on_error_timeout_ms", "u"),
    15: ("certificate_gossip_neighbors", "u"),
    16: ("standstill_min_egress_bytes_per_s", "u"),
}


def simplex(sl: Slice):
    tag = sl.u(8)
    if tag != 0x22:  # simplex_config#21 (v1) тут не разбираем
        return {"tag": hex(tag)}
    sl.u(5)  # flags
    res = {"protocol_version": sl.u(2), "use_quic": sl.bit(), "slots_per_leader_window": sl.u(32)}
    params = {}
    if sl.bit():  # HashmapE: есть корень
        for k, v in hashmap(sl.ref(0), 8).items():
            raw = v.u(32)
            name, kind = NONCRITICAL.get(k, (f"param_{k}", "u"))
            params[name] = struct.unpack(">f", struct.pack(">I", raw))[0] if kind == "f" else raw
    res["noncritical"] = params
    res["broadcasts"] = "plumtree (новые)" if res["protocol_version"] >= 2 else "старые"
    res["block_sync"] = res["protocol_version"] == 1
    return res


def p30(net):
    s = root_slice(net, 30)
    assert s.u(8) == 0x10
    out, k = {}, 0
    for name in ("mc", "shard"):  # new_consensus_config_all#10 mc:(Maybe ^) shard:(Maybe ^)
        if s.bit():
            out[name] = simplex(s.ref(k))
            k += 1
        else:
            out[name] = None
    return out


def p34(net):
    s = root_slice(net, 34)
    tag = s.u(8)
    r = {"tag": hex(tag), "utime_since": s.u(32), "utime_until": s.u(32), "total": s.u(16), "main": s.u(16)}
    r["since_utc"] = str(datetime.fromtimestamp(r["utime_since"], timezone.utc))[:16]
    r["until_utc"] = str(datetime.fromtimestamp(r["utime_until"], timezone.utc))[:16]
    return r


def config_link(net):
    d = http_json(f"{API[net]}/getMasterchainInfo")
    l = d["result"]["last"]
    rh = base64.b64decode(l["root_hash"]).hex().upper()
    fh = base64.b64decode(l["file_hash"]).hex().upper()
    return (
        f"{EXPLORER[net]}/config?workchain=-1&shard=8000000000000000"
        f"&seqno={l['seqno']}&roothash={rh}&filehash={fh}"
    ), l["seqno"]


def shards_now(net):
    d = http_json(f"{API[net]}/getMasterchainInfo")
    seqno = d["result"]["last"]["seqno"]
    d = http_json(f"{API[net]}/shards?seqno={seqno}")
    return [(s["workchain"], hex(int(s["shard"]) & 0xFFFFFFFFFFFFFFFF)) for s in d["result"]["shards"]]


# ---------------------------------------------------------------- ClickHouse


def ch_creds():
    user, pwd = os.environ.get("CH_USER"), os.environ.get("CH_PASS")
    if not (user and pwd):
        envf = os.path.join(os.path.dirname(__file__), "..", "configs-private", "loadtest.env")
        if os.path.exists(envf):
            for line in open(envf):
                line = line.strip()
                if line.startswith("CH_USER="):
                    user = user or line.split("=", 1)[1]
                elif line.startswith("CH_PASS="):
                    pwd = pwd or line.split("=", 1)[1]
    if not (user and pwd):
        sys.exit("нет кред ClickHouse: export CH_USER=... CH_PASS=... (или configs-private/loadtest.env)")
    return user, pwd


def ch(sql: str) -> str:
    user, pwd = ch_creds()
    url = CH_URL + "?" + urllib.parse.urlencode({"user": user, "password": pwd})
    req = urllib.request.Request(url, data=sql.encode())
    return urllib.request.urlopen(req, timeout=120).read().decode().rstrip("\n")


def ts(x: str) -> int:
    x = x.replace("T", " ")
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
        try:
            return int(datetime.strptime(x, fmt).replace(tzinfo=timezone.utc).timestamp())
        except ValueError:
            pass
    sys.exit(f"не разобрал время: {x} (нужно '2026-08-24 16:00' UTC)")


# ------------------------------------------------------------------ команды


def cmd_config(net):
    link, seqno = config_link(net)
    print(f"=== {net}  (мастерчейн seqno {seqno})")
    print("param 8   ", p8(net))
    for wc, v in p12(net).items():
        print(f"param 12  wc={wc}", v)
    print("param 22  (мастерчейн) ", limits(net, 22))
    print("param 23  (шарды)      ", limits(net, 23))
    print("param 29  ", p29(net))
    for k, v in p30(net).items():
        print(f"param 30  {k:5} ", v)
    print("param 34  ", p34(net))
    print("шарды сейчас:", shards_now(net))
    print("конфиг-ссылка (в таблицу):", link)


def cmd_diff():
    t, m = {}, {}
    for net, acc in (("testnet", t), ("mainnet", m)):
        acc["param8"] = p8(net)
        acc["param12_wc0"] = p12(net).get(0)
        acc["param22"] = limits(net, 22)
        acc["param23"] = limits(net, 23)
        acc["param30"] = p30(net)
        acc["param34"] = {k: v for k, v in p34(net).items() if k in ("total", "main")}
    for key in t:
        if json.dumps(t[key], sort_keys=True) == json.dumps(m[key], sort_keys=True):
            print(f"= {key}: одинаково")
        else:
            print(f"! {key}:\n    testnet: {t[key]}\n    mainnet: {m[key]}")


def cmd_status(net):
    db = CH_DB[net]
    print(f"=== {net} сейчас")
    print("шарды:", shards_now(net))
    print(
        ch(
            f"""select round(count()/900,2) as tps_15m from {db}.transactions
                where now >= toUnixTimestamp(now())-900 format TSV"""
        ),
        "tps за последние 15 мин (органика, если нагрузки нет)",
    )
    print(
        ch(
            f"""select workchain, uniqExact(shard) as shards, round(count()/900,3) as bps
                from {db}.blocks where gen_utime >= toUnixTimestamp(now())-900
                group by workchain order by workchain format TSV"""
        )
    )
    print("свежесть индекса:", ch(f"select max(gen_utime), now() from {db}.blocks format TSV"))


def cmd_run(a, b, net):
    db, t0, t1 = CH_DB[net], ts(a), ts(b)
    dur = t1 - t0
    print(f"=== {net} прогон {a} .. {b} UTC ({dur//60} мин)")
    print("-- свежесть индекса:", ch(f"select max(gen_utime), now() from {db}.blocks format TSV"))
    print("\n-- tps по минутам (max 15-min avg = метрика в таблицу):")
    print(
        ch(
            f"""select toStartOfMinute(fromUnixTimestamp(now)) as m, count() as tx
                from {db}.transactions where now between {t0} and {t1}
                group by m order by m format TSV"""
        )
    )
    print("\n-- max 15-min avg tps (скользящее окно):")
    print(
        ch(
            f"""select formatDateTime(fromUnixTimestamp(win_start),'%F %H:%i') as window_start,
                       round(cnt/900,1) as tps_15m
                from (
                  select win_start, count() as cnt from (
                    select now, arrayJoin(range(intDiv(now-{t0},60)-14, intDiv(now-{t0},60)+1)) as k,
                           {t0}+k*60 as win_start
                    from {db}.transactions where now between {t0} and {t1}
                  ) where win_start >= {t0} and win_start+900 <= {t1}
                  group by win_start
                ) order by tps_15m desc limit 3 format TSV"""
        )
    )
    print("\n-- блокрейт всего (осторожно: при сплитах shard-id разные, per-shard смотреть в таблице ниже):")
    print(
        ch(
            f"""select workchain, uniqExact(shard) as shard_ids_seen,
                       round(count()/{dur},2) as blocks_per_s
                from {db}.blocks where gen_utime between {t0} and {t1}
                group by workchain order by workchain format TSV"""
        )
    )
    print("\n-- по 5 мин: шарды / bps на шард / пустые блоки / tx на непустой блок (сплит и мерж видны здесь):")
    print(
        ch(
            f"""select toStartOfInterval(fromUnixTimestamp(gen_utime), interval 5 minute) as ts,
                       uniqExact(shard) as shards,
                       round(count()/300/uniqExact(shard),2) as bps_per_shard,
                       round(100*countIf(transaction_count=0)/count(),1) as empty_pct,
                       round(avgIf(transaction_count, transaction_count>0),0) as tx_per_block,
                       countIf(want_split) as want_split, countIf(want_merge) as want_merge
                from {db}.blocks where workchain=0 and gen_utime between {t0} and {t1}
                group by ts order by ts format TSV"""
        )
    )
    print("\n-- джеттоны: jtps, уникальные отправители, tx на трансфер (норма ~4.03; поехало => рвётся цепочка):")
    print(
        ch(
            f"""select round(j/{dur},1) as jtps, senders, round(t/j,2) as tx_per_transfer from (
                  select (select count() from {db}.jetton_transfers
                          where transaction_now between {t0} and {t1}) as j,
                         (select uniqExact(source) from {db}.jetton_transfers
                          where transaction_now between {t0} and {t1}) as senders,
                         (select count() from {db}.transactions
                          where now between {t0} and {t1}) as t
                ) format TSV"""
        )
    )
    gr = (
        "https://status.toncenter.com/d/ee18qqhytttkwa/overview?orgId=1"
        f"&from={datetime.fromtimestamp(t0-300, timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000Z')}"
        f"&to={datetime.fromtimestamp(t1+300, timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000Z')}"
        "&timezone=browser"
    )
    print("\nссылка на графану (колонка chart):", gr)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "config":
        cmd_config(sys.argv[2] if len(sys.argv) > 2 else "testnet")
    elif cmd == "diff":
        cmd_diff()
    elif cmd == "status":
        cmd_status(sys.argv[2] if len(sys.argv) > 2 else "testnet")
    elif cmd == "run":
        if len(sys.argv) < 4:
            sys.exit("netcheck.py run '<from UTC>' '<to UTC>' [testnet|mainnet]")
        cmd_run(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else "testnet")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
