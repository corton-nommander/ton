# TON load cockpit

A read-only cockpit for load tests, running as the same Go binary in two instances:

| Network | URL | TPS meter | Session stats |
| --- | --- | --- | --- |
| Mainnet | `http://devnet-log.toncenter.com:1900/` (browser alias `:1902`) | `127.0.0.1:8092` | `127.0.0.1:18000` |
| Testnet | `http://devnet-log.toncenter.com:1901/` | `127.0.0.1:8093` | `127.0.0.1:18001` |

`make cockpit` builds `go-spam/build/cockpit`. The binary serves one static HTML/CSS/JS page and
read-only `/api/snapshot`, `/api/history?span=15m|1h|6h|24h`, and `/api/health` endpoints. The
page repaints every second; individual collectors keep their own source timestamps. It never sends
transactions or changes the pool. TPS is measured exclusively by the existing tpsmeter daemons.
The mainnet meter uses `/etc/tpsmeter/mainnet-private.json`, a pool of the managed Toncenter
archive liteservers `tc-lsa-01/02/03` on their externally reachable IPs (port 20700). Its
systemd override is `/etc/systemd/system/tpsmeter-mainnet.service.d/10-private-ls.conf` with
`--rps 150 --workers 32`; the previous single public LS returned `429` and left the historical
store behind the chain head. TPS remains visibly stale until the meter catches up. The LS
addresses and public keys come from `toncenter_hosts_28.07.2026.md` in the operator's Downloads,
and the network's zero state is inherited from the original mainnet config.

## Read-only inputs

- Tpsmeter `/v1/status` (1 second) and `/v1/series` (history, cached 15 seconds).
- Tpsmeter `/v1/head` (2 seconds) reads only the latest masterchain header and its referenced
  basechain shard headers from the meter's block source (liteservers on mainnet/testnet). This
  live head is **independent of TPS processing progress**; `/v1/status`'s `coverage_end` reports
  how far the TPS walk has caught up. A failed LS head read retains the last good snapshot with
  its original collection time. Toncenter v3 indexing freshness is shown separately under API.
- Prometheus `toncenter-mainnet-ls` (15 seconds). Testnet uses the existing clock-skew-aware
  `tonlogs.tools_smart.sync_status` reader (60 seconds); missing retired hosts are marked unknown.
- `health.toncenter.com/api/v1` (30 seconds) for API, streaming, and freshness probes.
- Session-stats dashboard `/api/stats` (60 seconds), showing recent collator pressure.
- The mainnet/testnet Telegram bots on `tc-apps-01` POST a signed run snapshot every five seconds
  to the matching cockpit's `/internal/bot-status`. `/api/snapshot` exposes `bot` (mode, target
  req/s, counters, static TPS, sampled confirmed/finalized rates and latency) and `bot_fresh`.
  Streaming samples at most one transfer per second, so its delivery rates measure the sample,
  not the throughput of all generated transactions. If a bot or receiver is unavailable for 20 seconds,
  `bot_fresh` becomes false rather than incorrectly reporting idle.
  The panel below the GAS tape distinguishes send acceptance (`sent / (sent + failed)`) from
  sampled delivery. Its TPS aim is an estimate for direct jetton modes (4 tx/request) or an
  exact static-TPS setting; batched/variable modes show no invented TPS estimate.
- A single Toncenter v3 `/accountStates?address=...&include_boc=false` request for 20 sampled
  WalletSpam owners **and** their 20 prepaid jetton wallets (60 seconds). The same interior
  wallet IDs are sampled on every update; Acton's driver wallet 0 and terminal wallet `count`
  are excluded. The public pool configurations live in `go-spam/cmd/cockpit/config/`.

Source errors keep the most recent successful reading **with its original timestamp**. Never
interpret a missing measurement as zero TPS, zero balance, or a healthy shard.
The TPS instrument warns about a block/checkpoint mismatch only when the meter detected it
within the last 60 seconds. The meter still retains its lifetime coverage/rollback flags for
historical queries; an old mismatch alone does not make live TPS appear unverified.

## Deploy

Build for the x86_64 log host from a checkout with Go available:

```sh
GOOS=linux GOARCH=amd64 go -C go-spam build -o build/cockpit ./cmd/cockpit
```

Install the binary as `/usr/local/bin/cockpit`, the unit template as
`/etc/systemd/system/cockpit@.service`, and the public pool configs as
`/etc/cockpit/{mainnet,testnet}.json`. Create root-only
`/etc/cockpit/{mainnet,testnet}.env`, containing each network's `TONCENTER_API_KEY`.
Enable `cockpit@mainnet.service` and `cockpit@testnet.service`. The default ports are
1900 and 1901 respectively. `COCKPIT_EXTRA_LISTEN=:1902` exposes the same mainnet
instance on a browser-compatible alternative port (port 1900 is blocked by some clients).
Additional optional environment settings are
`COCKPIT_METER_URL`, `COCKPIT_V3_URL`, `COCKPIT_PROMETHEUS_URL`,
`COCKPIT_HEALTH_URL`, `COCKPIT_STATS_URL`, `COCKPIT_LOGS_PYTHON`, and
`COCKPIT_LOGS_MODULE`. Keys are sent only in `X-API-Key` request headers and
are never included in the browser payload.

For bot status, set a distinct `COCKPIT_BOT_STATUS_SECRET` in each root-only cockpit env file.
On the corresponding bot instance set `BOT_STATUS_NETWORK` to `mainnet` or `testnet`,
`BOT_STATUS_URL` to `http://devnet-log.toncenter.com:1902/internal/bot-status` for mainnet or
`http://devnet-log.toncenter.com:1901/internal/bot-status` for testnet, and the matching
`BOT_STATUS_SECRET`. The bot signs the exact JSON body and nanosecond timestamp with HMAC-SHA256;
the receiver checks network, freshness, signature, and replay order. Status posting has a two-second
timeout and is optional: failures never interrupt bot commands or load loops. Static TPS reports
reflect only runs known to the current bot process (a restart loses that in-memory tracking).

If an Acton contract, minter, or pool key changes, update the **public** config and its
`expected_wallet_0` guard. Derive the public key locally without exporting the private seed:

```sh
go -C go-spam run ./cmd/cockpit --print-pool-public ../jetton-prepaid/pool.key
```

The testnet pool may drive only one 100k-wallet subpool of a larger 2.5m pool. Until the bot
exposes its active window over a read-only status API, the fuel gauge describes a sample of the
**whole** testnet pool, not the currently active slice.

## Verification

```sh
go -C go-spam test ./cmd/cockpit
go -C go-spam vet ./cmd/cockpit
curl -fsS http://devnet-log.toncenter.com:1900/api/health
curl -fsS http://devnet-log.toncenter.com:1901/api/health
```

Check `updated_at` and `error` on every section of `/api/snapshot`; a 200 response from the
cockpit itself does not imply all upstream sources are healthy. Source configuration and
current quality flags are deliberately visible on the page.
