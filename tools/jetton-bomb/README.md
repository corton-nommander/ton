# jetton-bomb

This sidechain copy includes native payment spam, an execution-mode check, and a
bounded masterchain internal-message runner. Read [sidechain/README.md](sidechain/README.md) for
integration details and measured results. The optimized basechain does not
support ordinary TVM/jetton internal messages at global version 14 or above.

TON load-testing toolkit: simulate a mainnet user influx by generating the **cheapest possible jetton transfers** at high throughput. At scale the bottleneck is cost, not compute, so the whole design is optimized around the price of a single transfer. The on-chain **GRAM** unit is just TON renamed for these tests.

A second, adversarial vector stress-tests the *other* bottleneck — **CellDB state-load** — by poking the largest-state accounts on the network (see [Big-state stress test](#big-state-stress-test)).

Three toolchains live here:

- **Acton / Tolk** (`jetton-prepaid/`) — the **current** on-chain contracts: a prepaid jetton (`JettonMinter` / `JettonWallet`) and the self-replicating `WalletSpam` load-generator pool.
- **Go** (`go-spam/`) — the **current** load generator + Telegram bot that drives the Tolk contracts (embeds their compiled code).
- **TypeScript / Blueprint** (repo root) — the original FunC/Tolk contracts, wrappers, tests and TG bot. Mostly legacy now (see [Legacy contracts](#legacy-contracts-func--blueprint)), except `StaticTps`, which the Go bot still drives.

See [CLAUDE.md](CLAUDE.md) for the full architecture and [JETTON.md](docs/JETTON.md) for the legacy devnet guide.

## How the load test works

A pool of `WalletSpam` wallets is stood up from **one signed external** (`Reproduce` fans out in chunks). Each pool wallet owns its own **prepaid** jetton wallet — "born" with a balance, so it can transfer with no prior mint — and all of them are deployed in-chain during that same fan-out. To generate load, the Go bot signs one external per wallet; each external makes that wallet send **1 jetton to a random *other pool wallet***.

**One transfer = 4 transactions:**

```
WalletSpam ──ext──▶ op::transfer ─────▶ op::internal_transfer ─────▶ op::excesses ──▶ WalletSpam
  (signs)        (its jetton wallet)     (recipient jetton wallet)    (change returns)
```

## Cost & economics

All figures below are **measured on real testnet traces** (basechain config = mainnet) and cross-verified to the nanogram.

**Two cost metrics — don't conflate them:**

- **Trace fees** — GRAM burned by the network across the 4 tx. This is the throughput cost, comparable across designs.
- **Net drain** — GRAM actually lost from the pool per transfer. The attached value is a *float*: `response_address = owner`, so unused GRAM comes back as `op::excesses`. Drain = fees + whatever value does **not** return. **This is what depletes the wallets** — use it for capacity planning.

### The optimization ladder

| stage | fees / transfer | net drain / transfer | fees @ 90k |
| --- | --- | --- | --- |
| Baseline (FunC, `state_init` on `op::transfer`) | 0.00458 | ~0.0056 | 412.2 |
| Tolk rewrite + prepaid, random-stranger recipient (mode 0) | 0.001483 | 0.002263 | 133.4 |
| → recipient = a random **pool** wallet (mode 0) | 0.001483 | 0.002068 | 133.4 |
| → **bare init mode** (`/initmode 1`) — **final** | **0.001324** | **0.001544** | **119.1** |

**Final: `0.001324` GRAM fees / `0.001544` drain per transfer — a 3.46× (−71%) cut from the FunC baseline.** That is the floor for a 4-tx jetton transfer (two jetton-wallet computes + message forwarding + the external); going lower needs a different transfer mechanic entirely.

### Where the money goes (final trace, mode 1)

| tx | account | op | fee (GRAM) |
| --- | --- | --- | --- |
| 0 | WalletSpam | external → `op::transfer` | 0.000435 |
| 1 | sender jetton wallet | `op::transfer` (`0x0f8a7ea5`) | 0.000489 |
| 2 | recipient jetton wallet | `op::internal_transfer` (`0x178d4519`) | 0.000365 |
| 3 | WalletSpam | `op::excesses` (`0xd53276db`) | 0.000035 |
| | | **Σ fees** | **0.001324** |

### What each optimization did

- **Tolk rewrite + prepaid jetton wallets pre-deployed in-chain** (by `Reproduce`) — the live transfer never pays to deploy the sender's jetton wallet. The single biggest cut (`0.00458 → 0.001483` fees).
- **1-year storage reserve** instead of 5-year (`MIN_STORAGE_DURATION`) — shrinks the recipient's min-storage reserve ~5×.
- **Recipient = a random *pool* wallet** (not a stranger) — its jetton wallet is already deployed, so the ~`0.000195` GRAM storage reserve stays inside the pool instead of leaking to an address we don't control. Drain −`0.000195`, confirmed to the nanogram.
- **Bare init mode** (`/initmode 1`) — since pool recipients are pre-deployed, the ~855-byte recipient `state_init` is dropped from the `internal_transfer` (its forward fee falls `454,849 → 85,646` nano). Drain −`0.00052`.

### Budget planning

- **TPS = transactions/sec.** 1 transfer = 4 tx, so e.g. 1200 TPS = **300 transfers/sec**.
- A **5-minute run at 300 transfers/s = 90,000 transfers ≈ 119 GRAM (fees) / 139 GRAM (drain).**
- Each pool wallet funded with **0.19 GRAM does ~123 transfers** before it runs dry.
- A **5,000-wallet pool** (950 GRAM) sustains ~**615,000 transfers ≈ 34 min** of continuous 300/s load — roughly 6–7 five-minute runs between refuels.

### Gasless GRAM funding

Native gasless transfers use a smaller relay float than jetton transfers:
**`relayValue = GRAM_TRANSFER_VALUE + 0.001`**, so the default transfer of **0.001 GRAM**
needs **0.002 GRAM** on the `sint` relay instead of 0.012 (6× less).
This applies to the bot's `/gassless_gram` and the CLI's `highload-gram` / `gassless-gram` modes.

Verified with real testnet transfers on **2026-10-01**: the sender emitted exactly 0.001 GRAM,
its seqno advanced, and its old balance grew rather than funding the operation. Target-side
cost was **0.000618030 GRAM including 0.000286895 accrued storage rent** on the first transfer;
the maximum supported **123-byte comment** cost **0.000375409 GRAM** on the warmed sender.
The relayer pays its own external/forwarding fees on top of the 0.002 attached value.

Use **`GASSLESS_GRAM_RELAY_VALUE`** (alias `GASLESS_GRAM_RELAY_VALUE`) for a GRAM-specific override.
It takes precedence over the general `GASSLESS_RELAY_VALUE` / `GASLESS_RELAY_VALUE` env override;
the CLI's explicit **`--relay-value`** takes precedence over both. Jetton's default remains 0.012.
An existing general relay override still applies to GRAM unless a GRAM-specific override is set.

### Mandatory gasless batching

All bot and CLI gasless modes use the shared **`pkg/highload.Batcher`**. The CLI's `gassless-*`
names are aliases of the highload-relayed modes. Every batch, **including a single request**, follows:

```text
external → Highload → internal_transfer to itself → N funded sint requests → pool wallets
```

- Mainnet defaults to **four relayers** (one per top-two-bit quarter). For the testnet dispatch-queue
  experiment set `HIGHLOAD_SHARDS=16` (one per address nibble). These are the original 16 wallets:
  the four mainnet-selected addresses are testnet buckets 0/4/8/c; no existing addresses change.
- Each relayer collects requests in **500ms windows**, without staggered phases. A shared address
  limiter allows at most **2 external RPC attempts/s**, including retries, and survives bot stop/start.
- A batch contains at most **254 actions** and a **60 KiB full external BoC**. Overflow moves to the next
  window; queues are bounded. Empty windows send nothing.
- Retries resend the **same external/query_id** through that limiter. `/retry off` / `--no-retry` disable
  the batch retry. A target's seqno stays pending until observed on-chain; a timeout after dispatch
  does not release it. Requests cancelled before dispatch are excluded.
- `Sent`/`Failed` count logical gasless submissions. `Batches` counts accepted externals;
  `BatchSend`/`BatchRetry` count actual RPC attempts. Acceptance does not confirm downstream execution.
- The self-message carries **0.01 GRAM for its compute**; this credits the same highload again. Each
  target still receives its own relay float (GRAM default 0.002, jetton 0.012). CLI gasless jetton
  transfers attach **0.01** by default (`GASSLESS_JETTON_VALUE`; explicit `--value` wins).

Live testnet verification on **2026-10-01** completed without aborted transactions:
[one GRAM request, 4 transactions](https://testnet.tonviewer.com/transaction/1f9c827523c6e358ff47d8b98185281097c858521d59598c270da9544696c35c),
[one GRAM + one jetton request, 8 transactions](https://testnet.tonviewer.com/transaction/27fa5f45357a6bdbb1ccae7daa8341b6f23bb5d6c48231a7eb8eebc9bbf9848c).
Both targets advanced their seqno without spending their old GRAM balance. A GRAM-only batch contributes
**`2 + 2N` on-chain transactions**, so offered gasless/s and network TPS are different measurements.

Mainnet's current relayer set and funding addresses are listed in [MAINNET_HIGHLOADS.md](docs/MAINNET_HIGHLOADS.md).

### Sampled delivery monitoring (bot)

Configure `STREAMING_API_KEY` and `STREAMING_NETWORK=testnet|mainnet` on the **matching** bot process.
The streaming connection is independent of `/api` and `/config` (which only switch the sending backend).
When configured, monitoring is on by default; `STREAMING_ENABLED=off` starts it disabled, and
`/streaming on|off` toggles it live. `/status` shows connection state and the last transfer run's
delivery figures. The key is never printed or written to the repository.

The bot samples at most **one real transfer per second** from `/gram`, `/jetton`,
`/hamster_kombat`, fat GRAM/jetton and gasless GRAM/jetton. It subscribes to the selected recipient
before sending that sample and watches `actions/confirmed` and `actions/finalized` for 90 seconds,
matching the **normalized external trace hash**, action type, sender, recipient and (for jettons) minter.
Gasless transfers require
`parent_gasless_action`; when the address-filtered event also includes its `gasless_request`, they verify
that the parent request targets the sampled sender. Recipient-only subscriptions may omit that parent
action. Other sends never wait for the streaming API. A single connection subscribes to at most 90 unique recipients
(`STREAMING_MAX_ADDRESSES` may lower that cap to leave more headroom for other clients sharing the key).
`/stop` leaves pending samples observable for their remaining time; `/streaming off` closes the
connection and marks them interrupted. A confirmed sample remains subscribed for finalization until the
same 90-second deadline. `>90s` means **not observed in the sampling window**, not proof of an on-chain
failure. Confirmed and finalized p50/p80/p95/p99 are calculated independently from the latest 3600
observations of each finality in that run. The rich `delivery` spoiler shows confirmed p80 in its title
and both finality sets in its table; the edited audit message (when `AUDIT_CHAT_ID` is set) has separate
text lines for confirmed and finalized p50/p80/p99.
The `streaming.Result`/`streaming.Sink` boundary permits a future SQLite or ClickHouse writer; the bot
currently retains only bounded in-memory statistics.

## Current stack

### Acton / Tolk contracts (`jetton-prepaid/`)

Requires the `acton` CLI on `PATH` (`export PATH="$HOME/.acton/bin:$PATH"`):

```bash
cd jetton-prepaid
acton build                                   # compile -> build/<Name>.json
acton test                                    # Tolk sandbox tests
acton script scripts/deploy.tolk --net testnet   # minter + full WalletSpam pool from one signed external
acton script scripts/destroy.tolk --net testnet  # tear the pool down (in-chain jetton withdraw + roll-up to a collector)
```

The pool keypair is a raw 32-byte ed25519 seed auto-generated to `jetton-prepaid/pool.key` on first deploy (gitignored). Devnet `start`/`start-bulk` rotate this key for the new chain; subsequent spam runs in that chain reuse the key and skip funding/deployment when the pool is ready. `acton script scripts/genkey.tolk` bootstraps the key without deploying. Deploy levers are env vars: `WALLETSPAM_COUNT`, `WALLET_VALUE`, `JETTON_VALUE`, `DEPLOY_CHUNK`, `DEPLOY_JETTON`.

### Go load generator + Telegram bot (`go-spam/`)

```bash
cd go-spam && go build ./...        # cmd/bot, cmd/spam
go test ./...                       # incl. cross-toolchain address parity
```

Run the bot from the repo root (`pool.key` auto-resolves to `./jetton-prepaid/pool.key`):

```bash
WALLET_MINTER=<minter from deploy> POOL_SIZE=<deployed count> \
JETTON_TRANSFER_VALUE=0.01 \
TELEGRAM_BOT_TOKEN=... WALLET_SECRET=<hex ed25519 seed> \
  ./go-spam/build/bot
```

Key knobs: `WALLET_MINTER` (part of every pool address — must match the deploy), `POOL_SIZE` (subwallet count), `JETTON_TRANSFER_VALUE` (float attached per transfer, default 0.05), and the `/initmode 0|1` command (**use `1`** for the cheapest path — pool recipients are pre-deployed). Backend is swappable live via `/api <url>` or `/config`. `/check_balance on` gates sends on the sender wallet's balance — skips wallets drained below the fee floor (counted `Skipped`, not `Failed`) and auto-stops at 75% drained, so a mainnet pool running dry doesn't bleed gas on doomed sends. Full env/command table in [CLAUDE.md](CLAUDE.md).

## Big-state stress test

A second load vector: instead of cheap transfers, **poke the accounts with the largest on-chain state** and watch collation. When a validator collates a block touching an account it must load that account's cells from CellDB, so hammering big-state accounts tests whether *state-load cost* — not compute — throttles a shard.

**Result:** a mainnet **shard split was triggered by ~1 TPS** of 1-nanogram, bounceable, empty-body pokes at big-state accounts. It is **timing/state-dependent**, though — a run that hit a 162 KB account didn't split while one topping out at 82 KB did — so it needs the network in a susceptible state, not just one fat account touched.

Pipeline:

1. **Find the biggest states** — `ton-index-go/cmd/bigstate` scans the toncenter kvrocks account-state store (ranking by serialized state size, partitioned by address prefix), writing `big_accounts.txt` (raw `wc:hex`, size-sorted biggest-first) + a `.sizes` sidecar.
2. **Poke them** — each pool wallet sends one 1-nanogram bounceable internal (empty body) to a target:

   ```bash
   ./go-spam/build/spam --mode targets --targets ./big_accounts.txt \
     --order sequential \                 # biggest-first (deterministic), or 'random'
     --count 5001 --minter <minter> \
     -e https://toncenter.com/api/v2 -k <key> \
     --check-balance --freq 1
   ```

   Every poke is logged to stdout **and `spam.log`** (sender wallet / target / state size / ok|FAIL), so the last account poked is always on record — no reconstructing from chain data after the fact.
3. **Reconstruct** (if needed) — `cmd/pokes` pulls the poke history (target + tx hash + size, chronological) back off the toncenter v2 API.

The 1 nanogram bounces back, so cost ≈ fees only; the point is that collating each poke forces the recipient's cells to load. See [CLAUDE.md](CLAUDE.md) for the full flag/env table.

### Reproducer contracts (`fatbastard.fc` / `humpalot.fc`)

Two throwaway FunC contracts (repo root, driven by the Go spammer) that reproduce the two mainnet memecoin-bot behaviours **in isolation**, so each split axis can be exercised on its own instead of poking someone else's accounts:

- **`contracts/fatbastard.fc`** — the *state-load* axis. `grow` inflates the contract's **own** storage to N unique cells at a chosen tree depth; `poke` (any body < 32 bits) just bumps a nonce so the storage-dict hash changes, forcing the collator to re-walk the whole tree (the per-run `StorageStatCache` misses). That collation-**time** cost in `prelim_storage_stat` — not gas — is what drove the late-July `long_collation` splits. Grow it fat, then stream cheap pokes.
- **`contracts/humpalot.fc`** — the *gas/CPU* axis. `grind` burns unrolled `HASHEXT_SHA256` (MEV minimal-hash search) until it hits a caller-supplied gas budget, self-adapting to the network's per-tx gas limit; `grind_loop` re-posts one message to itself to sustain a chain of full-gas transactions. Measures where TVM `trx_tvm` approaches the ~400 ms collation budget — the load that tells us whether the basechain gas limit can be raised.

Both live on the legacy Blueprint side: wrappers `wrappers/{Fatbastard,Humpalot}.ts`, deploy scripts `yarn run deployFatbastard` / `deployHumpalot`, tests `tests/LoadTest.spec.ts`.

---

## Legacy contracts (FunC / Blueprint)

> The sections below document the **original** FunC/Tolk contracts, wrappers and Blueprint scripts at the repo root. They are superseded by the Acton/Tolk + Go stack above (except `StaticTps`, still driven by the Go bot), but kept for reference.

## Quickstart

```bash
yarn install
yarn build
yarn test
```

Run any Blueprint script:

```bash
yarn run <scriptName>
```

Examples:

```bash
yarn run deployWalletSpam
yarn run reproduce
yarn run deployPingPong
yarn run deployStaticTps
```

## Repository Layout

- `contracts/` - all smart-contract sources and shared contract modules.
- `wrappers/` - TypeScript wrappers for deploy/send/get methods.
- `scripts/` - deployment and operational scripts.
- `tests/` - Jest tests with TON sandbox.
- `build/` - compiled artifacts.

## Contract Index

| File | Type | Wrapper | Main scripts |
| --- | --- | --- | --- |
| `contracts/wallet_spam.fc` | Deployable | `wrappers/WalletSpam.ts` | `deployWalletSpam`, `reproduce`, `sendBomb`, `jettonSpam`, `sendShardBomb`, `spamMonitor`, `produceMonitor`, `completeBomb`, `sendSimple` |
| `contracts/wallet_r.fc` | Deployable | `wrappers/WalletR.ts` | `deployOne`, `sendProduce`, `searchFailed` |
| `contracts/fatbastard.fc` | Deployable | `wrappers/Fatbastard.ts` | `deployFatbastard` |
| `contracts/humpalot.fc` | Deployable | `wrappers/Humpalot.ts` | `deployHumpalot` |
| `contracts/static_tps.tolk` | Deployable | `wrappers/StaticTps.ts` | `deployStaticTps`, `stopPingPong` |
| `contracts/ping_pong.tolk` | Deployable | `wrappers/PingPong.ts` | `deployPingPong` |
| `contracts/jetton-minter.func` | Deployable | `wrappers/JettonMinter.ts` | `deployJettonMinter`, `minterController`, `justMint` |
| `contracts/jetton-wallet.func` | Deployable | `wrappers/JettonWallet.ts` | `deployJettonWallet`, `sendBomb`, `sendShardBomb`, `justSendJetton`, `justSendCurrency` |
| `contracts/jetton-utils.func` | Shared module | n/a | imported by jetton contracts |
| `contracts/messages.func` | Shared module | n/a | imported by wallet and jetton contracts |
| `contracts/op-codes.func` | Shared module | n/a | imported by jetton contracts |
| `contracts/error-codes.func` | Shared module | n/a | imported by jetton contracts |
| `contracts/params.func` | Shared module | n/a | imported by jetton contracts |
| `contracts/w5.fc` | Reference contract | n/a | no script integration |
| `contracts/stdlib.func` | Standard lib copy | n/a | included by FunC contracts |
| `contracts/imports/stdlib.func` | Standard lib copy | n/a | included by FunC contracts |

---

## `contracts/wallet_spam.fc` (WalletSpam)

### What it does
- Signature-based wallet with `seqno`, `subwallet_id`, and `pubkey`.
- Supports chain deployment (`op::reproduce = 0xba5e5d50`) of many derived subwallets.
- Stores an embedded `spam_msg` cell and sends it on each reproduce call.
- Has optional extra-currency handling hooks (partly commented in contract).

### Key logic
- `DEPLOY_CHUNK = 200` child deploys per internal reproduce transaction.
- `MIN_BALANCE = 1000 TON` reserved on each deployment hop.
- Validates sender by recomputing expected previous subwallet address.
- Reads config param `-137` and throws `666` when spam mode is disabled.

### Reproduction scheme

```text
WalletSpam[sub=i] receives op::reproduce(last, from=i-1)
  -> validates sender == WalletSpam[sub=i-1]
  -> emits stored spam_msg
  -> deploys WalletSpam[sub=i+1], WalletSpam[sub=i+2], ... (up to DEPLOY_CHUNK)
  -> on final child in chunk, forwards op::reproduce(last, from=i)

Result: a chained fanout that deploys large subwallet ranges.
```

### Usage
- Deploy seed wallet: `yarn run deployWalletSpam`
- Start chain deployment: `yarn run reproduce`
- Monitor deployment progress: `yarn run spamMonitor` or `yarn run produceMonitor`
- Generate jetton spam load: `yarn run jettonSpam` (see [JETTON.md](docs/JETTON.md))
- Generate load with many deployed subwallets: `yarn run sendBomb`, `yarn run sendShardBomb`
- Retry failures from file: `yarn run completeBomb`

### Tests
- `tests/WalletSpam.spec.ts`

---

## `contracts/wallet_r.fc` (WalletR)

### What it does
- Lighter version of reproducing wallet without embedded spam payload.
- Same signed external wallet flow and chained reproduce mechanism.

### Key logic
- `deploy_chunk = 200`
- `min_balance = 1 TON`
- Internal op `0xba5e5d50` for recursive subwallet deployment.
- Sender validation uses deterministic address recomputation.

### Reproduction scheme

```text
WalletR[sub=i] -> deploy next WalletR children -> forwards reproduce to continue range
```

### Usage
- Deploy seed wallet: `yarn run deployOne`
- Reproduce range: `yarn run sendProduce`
- Check missing/non-active derived jetton wallets: `yarn run searchFailed`

### Tests
- `tests/WalletR.spec.ts`

---

## `contracts/static_tps.tolk` (StaticTps)

### What it does
- Maintains fixed-rate transaction pressure by periodically scheduling message bursts.
- Uses bounce wakeups plus fanout `ContinueMsg` children that emit `WorkMsg` messages.

### Message interfaces
- `RunMsg` (`0x5ce7c1d2`): set `targetTps` and start/update run.
- `StopMsg` (`0x07c32b3f`): stop scheduler.
- `ContinueMsg` (`0xc0c0c0c0`): carries `{ epoch, quota }`.
- `WorkMsg` (`0x10adbeef`): minimal tx payload.

### Current behavior (as implemented now)
- `MAX_BATCH_SIZE = 250`, `WINDOW_SIZE = 30`, `MAX_WINDOW_TX = 62500`.
- On each eligible bounce (`elapsed >= WINDOW_SIZE`):
  - computes `toSend = targetTps * WINDOW_SIZE`, capped by `62500`.
  - splits into at most `250` children with filler distribution (`base + remainder`).
  - each child emits `quota` work messages.
- `RunMsg` sets `windowStart = now - WINDOW_SIZE + 5`, so first major scheduling window can trigger quickly after start.
- `activeEpoch` is tracked and incremented, but epoch filtering in `ContinueMsg` handler is currently commented out, so old-epoch children are not rejected.

### Scheduler scheme

```text
Run -> sendPing(to empty masterchain addr)
Bounce -> onBouncedMessage
  -> if window elapsed: build N ContinueMsg children (N <= 250)
ContinueMsg child -> send quota WorkMsg to fixed zero-like address
Always -> send next ping
```

### Usage
- Deploy + start: `yarn run deployStaticTps`
- Stop running instance: `yarn run stopPingPong` (script name is legacy; it sends `StopMsg` to `StaticTps`)
- Wrapper getters: `isRunning`, `targetTps`, `windowTxCount`, `totalTxCount`, `activeEpoch`, `maxTps`, `getInfo`

### Tests
- `tests/StaticTps.spec.ts`

---

## `contracts/ping_pong.tolk` (PingPong)

### What it does
- Minimal bounce loop for controlled transaction iteration counting.
- Sends ping to non-existent address and continues on bounce until `limit`.

### Message interfaces
- `RunMsg` (`0x5ce7c1d2`) with `limit`.
- `StopMsg` (`0x07c32b3f`).

### Loop scheme

```text
Run(limit)
  -> sendPing(counter=0)
bounce
  -> counter += 1
  -> if counter >= limit: stop
  -> else sendPing(counter)
```

### Usage
- Full interactive flow: `yarn run deployPingPong`
- Actions in script: deploy new, run existing, stop existing, status

### Tests
- `tests/PingPong.spec.ts`

---

## `contracts/jetton-minter.func` (JettonMinter)

### What it does
- Jetton master contract with minting, wallet discovery, admin rotation, and content updates.
- Stores total supply, admin, metadata/content cell, and wallet code.

### Supported ops
- `op::mint` (`0x1674b0a0`)
- `op::provide_wallet_address` (`0x2c76b973`)
- `op::change_admin` (`0x4840664f`)
- `op::change_content` (`0x5773d1f5`)
- Handles `op::burn_notification` from valid jetton wallets

### Minting scheme

```text
Admin -> JettonMinter (mint)
  -> computes recipient jetton wallet address/state_init
  -> sends internal_transfer to that wallet
  -> increments total_supply
```

### Usage
- Deploy new minter: `yarn run deployJettonMinter`
- Interactive admin/user actions: `yarn run minterController`
- Quick mint script: `yarn run justMint`
- Useful wrapper calls:
  - `getJettonData()`
  - `getWalletAddress(owner)`
  - `sendMint(...)`
  - `sendChangeAdmin(...)`
  - `sendChangeContent(...)`

### Tests
- `tests/JettonMinter.spec.ts`

---

## `contracts/jetton-wallet.func` (JettonWallet)

### What it does
- Token wallet that handles transfers, incoming transfers, burns, and bounce recovery.
- Includes extra maintenance ops: withdraw TON and withdraw jettons.

### Supported ops
- `op::transfer` (`0x0f8a7ea5`)
- `op::internal_transfer` (`0x178d4519`)
- `op::burn` (`0x595f07bc`)
- `op::withdraw_tons` (`0x6d8e5e3c`)
- `op::withdraw_jettons` (`0x768a50b2`)

### Transfer scheme

```text
Owner -> sender JettonWallet (transfer)
  -> sender wallet checks ownership/balance
  -> sends internal_transfer to recipient wallet
recipient wallet
  -> credits balance
  -> optionally sends transfer_notification to owner
  -> optionally sends excesses to response address
```

### Usage
- Deploy wallet template/instance: `yarn run deployJettonWallet`
- High-load jetton sending via WalletSpam: `yarn run sendBomb`, `yarn run sendShardBomb`
- Single transfer: `yarn run justSendJetton`
- Wrapper helpers:
  - `transferMessage(...)`
  - `sendTransfer(...)`
  - `sendBurn(...)`
  - `sendWithdrawTons(...)`
  - `sendWithdrawJettons(...)`

### Tests
- `tests/JettonWallet.spec.ts`

---

## `contracts/jetton-utils.func`

### What it does
- Shared address/state-init helpers for jetton contracts.
- Uses `SPLIT_DEPTH = 4` shard-aware address shaping.
- Provides `calculate_jetton_wallet_state_init`, `calc_address`, `calc_user_wallet`, and packing helpers.

### Usage
- Imported by `jetton-minter.func` and `jetton-wallet.func`.
- Not deployed standalone.

---

## `contracts/messages.func`

### What it does
- Shared message flags, send modes, and builder helpers for raw message serialization.
- Defines helpers like `store_msgbody_prefix_stateinit` and bounce-prefix skip.

### Usage
- Imported by wallet and jetton contracts.
- Not deployed standalone.

---

## `contracts/op-codes.func`

### What it does
- Centralized opcode constants and small load/store helpers for op/query-id fields.

### Usage
- Imported by jetton contracts.
- Keep wrappers in sync with constants when adding new operations.

---

## `contracts/error-codes.func`

### What it does
- Shared numeric error codes for access control, fee checks, and unknown actions.

### Usage
- Imported by jetton contracts.
- Useful for debugging failed transactions and test assertions.

---

## `contracts/params.func`

### What it does
- Shared workchain and address-resolvability checks.
- Provides `force_chain` guard.

### Usage
- Imported by jetton contracts to enforce expected workchain.

---

## `contracts/w5.fc`

### What it does
- Full Wallet V5 style extensible wallet implementation (reference-grade contract).
- Includes extension management and signed/internal request processing logic.

### Usage
- Currently not wired into wrappers/scripts in this repo.
- Useful as a reference when comparing custom wallet behavior against V5 patterns.

---

## `contracts/stdlib.func`

### What it does
- Local copy of FunC standard library primitives.

### Usage
- Included directly by several contracts.

---

## `contracts/imports/stdlib.func`

### What it does
- Same standard library content under `imports/` path for include compatibility.

### Usage
- Included by contracts that reference `imports/stdlib.func`.

---

## Standalone Scripts

| Script | Description |
| --- | --- |
| `scripts/tpsCli.ts` | Generate StaticTps deploy/run/stop BOCs without a provider (`ts-node scripts/tpsCli.ts`) |
| `scripts/jettonSpam.ts` | Continuous jetton transfer spam from random wallets (`COUNT=N FREQ=F yarn run jettonSpam`) |
| `scripts/telegramTpsBot.ts` | Telegram bot for TPS and jetton spam control (`yarn bot:tps`) |

See [JETTON.md](docs/JETTON.md) for a step-by-step devnet guide on wallet deployment and jetton spam.

## Script Notes

- Most scripts use mnemonic from `mnemonic.txt` (`scripts/lib/mnemonic.ts`).
- `justSendJetton` and `justSendCurrency` use `WALLET_MNEMONIC` env var.
- Keep mnemonic files private and never commit them.

## Test Commands

```bash
yarn test tests/WalletSpam.spec.ts
yarn test tests/WalletR.spec.ts
yarn test tests/PingPong.spec.ts
yarn test tests/StaticTps.spec.ts
yarn test tests/JettonMinter.spec.ts
yarn test tests/JettonWallet.spec.ts
```
