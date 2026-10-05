# jetton-bomb → devnet integration handoff

Audience: the agent wiring `jetton-bomb` into the devnet `ton` orchestration script (the one whose
`jetton-spam` / `externals-spam` subcommands drive jetton-bomb through **blueprint** today).

**TL;DR:** replace the legacy TS/Blueprint flow with the **Go-only** stack. The devnet host needs **just
the Go toolchain** — no acton, no node/npm/blueprint, no minter contract, no deployer wallet, no
toncenter v3. Three Go binaries (`make` builds them): `deploy` (stand up the pool), `spam` (drive
jetton/external load), `bot` (optional Telegram). Everything talks to the existing devnet
`ton-http-api` (toncenter **v2**). The pool deploy is byte-identical to the acton deploy, asserted by a
parity test against a Tolk-generated vector.

---

## 1. The three toolchains (what's current vs legacy)

| Toolchain | Location | Role on the devnet |
|---|---|---|
| **Go** (`go-spam/`) | `deploy` + `spam` (+ `bot`) | **THE devnet stack.** Native pool deploy + load. Talks to `ton-http-api` v2. |
| **Acton / Tolk** (`jetton-prepaid/`) | the actual contracts (compiled to code blobs) | **Dev-time only.** Compiles the Tolk contracts; the Go binaries embed the compiled blobs (`pkg/walletspam/code.go`). **Not installed on the devnet host.** Regenerate blobs + parity vectors only when contracts change. |
| **TS / Blueprint (FunC)** (root `scripts/`, `contracts/`) | `deployWalletSpam.ts`, `jettonSpam.ts` | **LEGACY** — what the `ton` script uses now. Old FunC contracts, per-transfer `state_init`, expensive. Being replaced. |

---

## 2. What the devnet `ton` script does today (legacy path, to be replaced)

`jetton-spam <num-wallets> <freq> [ls]` / `externals-spam …` currently: forces `blueprint.config.ts`
endpoint; node22 + `npm install` + blueprint upgrade; derives+funds a v4r2 deployer via
`topup_wallet_from_main_wallet` (**top-up `NUM_WALLETS*1005 TON`**); `blueprint run deployWalletSpam`
(marker `Sent reproduce call`); reads generated `mnemonic.txt`; `blueprint run jettonSpam` /
`ts-node jettonSpamLs.ts` (`ONLY_EXT=1` for externals). All of that is replaced below.

---

## 3. The new deploy — `go-spam/cmd/deploy` (native, no acton)

The pool comes up in **two on-chain steps**, and the `ton` script drives both (it already owns the
genesis main-wallet funding path):

1. **Fund wallet0.** `deploy plan` prints wallet0's address + the exact funding amount. The script funds
   that address from the genesis `main-wallet` (your existing `topup_wallet_from_main_wallet`). wallet0
   stays *uninitialized*, just holding balance.
2. **Kick off.** `deploy broadcast` sends ONE pool-key-signed external to wallet0 that **carries
   wallet0's own state_init** — so the single external both *deploys* wallet0 and runs its
   `onExternalMessage`, which fires wallet0's prepaid-jetton deploy + the `Reproduce` that replicates the
   rest of the pool in-chain (chunked). No deployer wallet, no separate deploy tx.

**No minter contract is deployed.** Prepaid jetton transfers never call the minter — its address is only
a storage parameter that fixes the pool/jetton addresses. Pick any minter address and use the **same**
one for `deploy` and `spam` (default `SpamMinterTON`, already shared by all three binaries).

**Byte-parity:** the kickoff external (`pkg/deploy.Deployer.KickoffBody`) is asserted byte-for-byte
against the acton `buildKickoffExternal` for a fixed vector (`pkg/deploy/parity_test.go` vs
`jetton-prepaid/scripts/_deploy_parity.tolk`), plus a guard that wallet0's state_init hashes to its
address (required or TON rejects the deploy).

**`deploy` CLI:**
```
deploy plan       # generates pool.key on first run; prints eval-able:
                  #   DEPLOY_WALLET0=<addr>  DEPLOY_FUNDING_NANO=<n>  DEPLOY_MINTER=<addr>
                  #   DEPLOY_POOL_PUBKEY=<hex>  DEPLOY_COUNT=<n>       (NOTE about key gen → stderr)
deploy broadcast  # sends the kickoff external (with wallet0 state_init), waits for wallet0 seqno->1,
                  # THEN waits for the TERMINAL wallet (subwallet count), and only then prints "DEPLOY DONE".
```
**Readiness wait (why `broadcast` blocks a bit).** wallet0 seqno→1 only means wallet0 accepted the kickoff;
the chunked `Reproduce` fan-out materializes the rest over the next blocks. `broadcast` waits for the
**terminal wallet** (subwallet `count`) — the causally-LAST wallet in the reproduce chain, deployed only
after the whole chain unrolled — then prints `DEPLOY DONE` / exits 0. Its deployment means the **entire pool
is up**, so the spammer never carries a permanent failed% from an unbuilt tail. Each poll it probes ~20
sample subwallets spread across `[0,count]` and draws a **progress bar** (`\r`-redrawn: filled %, ~X/count
wallets, elapsed) so a multi-minute wait shows movement instead of looking hung; the terminal (last sample)
is the completion signal, the rest are UX. The probe is `get_wallet_data` on the prepaid jetton wallet when
`DEPLOY_JETTON=1`, else `seqno` on the WalletSpam (readiness = get-method returns without error and a
non-empty stack). **On timeout it exits non-zero** → the script aborts before spamming. If the terminal
never deploys, the chain ran out of **fee cushion** — raise `--fee-slack`/`FEE_SLACK` (see Funding below).

Flags/env (nano = nanoGRAM; same names/defaults as the acton deploy env):
`-c/--count`/`COUNT`, `--chunk`/`DEPLOY_CHUNK` (default 200), `--deploy-jetton`/`DEPLOY_JETTON` (default 1),
`--wallet-value`/`WALLET_VALUE` (100000000000 = **100 GRAM/wallet**, devnet default — grams are free there),
`--jetton-value`/`JETTON_VALUE` (10000000),
`--fee-slack`/`FEE_SLACK` (100000000 = **0.1 GRAM/wallet** fee cushion — devnet default; was 0.01, which
starved the tail on 10k pools), `--minter`/`WALLET_MINTER`, `-m/--pool-key`/`POOL_KEY_FILE`
(default `jetton-prepaid/pool.key`), and for `broadcast`: `-e/--endpoint`/`TONCENTER_ENDPOINT` (v2 root)
or `-l/--ls-config`/`LS_CONFIG`, `-k/--api-key`/`TONCENTER_API_KEY`. Readiness-wait knobs (broadcast only):
`--pool-timeout`/`POOL_READY_TIMEOUT` (default 10m — raise it if a big pool's chain takes longer),
`--poll-interval`/`POOL_POLL_INTERVAL` (default 3s).

**Funding (single lever):** `(count+1) × (WALLET_VALUE+FEE_SLACK)`. With the devnet default
(`WALLET_VALUE=100 GRAM`, `FEE_SLACK=0.1`) that's `(count+1) × 100.1 GRAM`. For 5000 wallets ≈ **500,600
GRAM** (`DEPLOY_FUNDING_NANO=500600100000000`) — irrelevant on devnet where grams are free. `FEE_SLACK` is
the rolling fee cushion (real cascade cost ~0.02/wallet); too low starves the tail so the terminal never
deploys — 0.1 gives a fat 5x margin (excess rolls to the tail wallet, recoverable). `plan` prints
the exact `DEPLOY_FUNDING_NANO`; don't hand-compute it. (On a metered net, drop `WALLET_VALUE` to taste.)

---

## 4. The load generator — `go-spam/cmd/spam`

Headless, two modes (byte-identical to the bot's `/jetton` and `/external`):
- `--mode jetton` (**default**): each pool wallet signs an external carrying one `op::transfer` of 1
  jetton to a **random other pool wallet** (GRAM stays in the pool). Shared `pkg/jetton`, golden-tested.
- `--mode external` (or `ONLY_EXT=1`): empty signed externals (seqno bump only) — pure external load.

Positional `count freq` or flags/env: `COUNT/-c`, `FREQ/-f` (or `PERIOD/-p`), `MODE/--mode`,
`JETTON_TRANSFER_VALUE/--value` (default 0.05), `JETTON_INIT_MODE/--init-mode` (0 full state_init |
**1 bare/cheapest** — recipients are pre-deployed, use 1), `TONCENTER_ENDPOINT/-e` (v2 root) **or**
`LS_CONFIG/-l`, `TONCENTER_API_KEY/-k`, `POOL_KEY_FILE/-m`, `WALLET_MINTER/--minter`, `SEND_TIMEOUT/-t`,
`MAX_INFLIGHT`, `ERROR_LOG`, `NO_RETRY/--no-retry` (single-attempt sends: no refetch+resend on failure,
the pre-"Retries" behavior — retries are on by default). It needs **no** `NETWORK_GLOBAL_ID` (only the
bot's V5R1 *control* wallet uses `-217`; `spam` drives only pool externals, which don't embed the network id).

**Endpoint:** the Go HTTP backend is **toncenter v2 REST**. Give it the **v2 root** — i.e. the devnet
`ton-http-api` URL you use for `/jsonRPC`, minus the `/jsonRPC` suffix (auto-stripped anyway):
`TONCENTER_ENDPOINT=http://devnet-01.toncenter.com`. It POSTs to `/sendBoc`, `/runGetMethod`,
`/getAddressInformation` — confirm those resolve at that root (they do if `/jsonRPC` did).

---

## 5. Rewriting the `ton` script's `jetton-spam | externals-spam` block — step by step

Replace the whole legacy body (its `[1/9]…[9/9]` steps) with the following. It **reuses your existing
helpers** (`topup_wallet_from_main_wallet`, `JETTON_MAIN_WALLET_SEQ_FILE`, `pick_first_existing_dir`,
the PID-file pattern, `stop_jetton_spam_global`, `send_to_telegram`). Devnet constants:

```bash
DEVNET_ENDPOINT="http://devnet-01.toncenter.com"        # v2 root (your /jsonRPC URL minus /jsonRPC)
DEVNET_MINTER="kQCy3cWuUXredDmXUTipDo31Juda2Y10jfK9DYS0gBPAjD-X"   # any fixed addr; NOT deployed
```

**Step 0 — prereqs.** Ensure `go` is on PATH (no node/npm/blueprint/acton). Nothing else to install.

**Step 1 — args + mode.** Keep your existing parse of `NUM_WALLETS`, `FREQ`, and the
`jetton-spam`↔`externals-spam` distinction. Set:
```bash
if [[ "$CMD" == "externals-spam" ]]; then SPAM_MODE="external"; else SPAM_MODE="jetton"; fi
```

**Step 2 — resolve dir + build the Go binaries** (replaces node22/npm/blueprint):
```bash
JETTON_BOMB_DIR=$(pick_first_existing_dir "$PROJECT_GENESIS_DIR/jetton-bomb" "$GENESIS_DIR/jetton-bomb") \
  || { echo "jetton-bomb dir not found"; exit 1; }
make -C "$JETTON_BOMB_DIR"                      # builds go-spam/build/{deploy,spam,bot}
DEPLOY_BIN="$JETTON_BOMB_DIR/go-spam/build/deploy"
SPAM_BIN="$JETTON_BOMB_DIR/go-spam/build/spam"
POOL_KEY="$JETTON_BOMB_DIR/jetton-prepaid/pool.key"
```

**Step 3 — fresh pool per run** (mirrors the legacy `NEW=1` fresh-wallets behavior; skip this line to
keep a stable pool across runs):
```bash
rm -f "$POOL_KEY"
```

**Step 4 — plan: generate the key + compute wallet0 and funding** (no network):
```bash
eval "$(
  POOL_KEY_FILE="$POOL_KEY" "$DEPLOY_BIN" plan \
    --count "$NUM_WALLETS" --minter "$DEVNET_MINTER"
)"
# now set: DEPLOY_WALLET0, DEPLOY_FUNDING_NANO, DEPLOY_MINTER, DEPLOY_POOL_PUBKEY, DEPLOY_COUNT
echo "$JETTON_STEP_PREFIX wallet0=$DEPLOY_WALLET0 funding_nano=$DEPLOY_FUNDING_NANO"
```
(`plan`'s stdout is only `KEY=VALUE` lines — safe for `eval`. The "generated pool key" NOTE goes to
stderr. Back up `pool.key` if you keep a stable pool — it controls the whole pool.)

**Step 5 — fund wallet0 from the genesis main-wallet** (your existing helper; it already waits ~15s for
chain apply). Fund **wallet0**, not a deployer, with the amount `plan` computed:
```bash
topup_wallet_from_main_wallet "$DEPLOY_WALLET0" "$DEPLOY_FUNDING_NANO" \
  "$JETTON_MAIN_WALLET_SEQ_FILE" "$JETTON_STEP_PREFIX" \
  || { echo "fund wallet0 failed"; exit 1; }
```

**Step 6 — broadcast the kickoff + wait for the terminal wallet** (deploys wallet0, fires the reproduce
chain, then **waits for subwallet `count`** — the end of the chain):
```bash
POOL_KEY_FILE="$POOL_KEY" "$DEPLOY_BIN" broadcast \
  --count "$NUM_WALLETS" --minter "$DEVNET_MINTER" \
  --endpoint "$DEVNET_ENDPOINT" \
  || { echo "kickoff broadcast/verify failed"; exit 1; }
# blocks until "DEPLOY DONE" — the terminal wallet (subwallet count) is up, i.e. the WHOLE pool deployed,
# so the spammer starts against a complete pool (no lingering failed%). If it times out, the tail starved:
# raise FEE_SLACK (fee cushion). Raise --pool-timeout only if a big pool's chain legitimately takes >10m.
```
(Must pass the **same** `--count` and `--minter` as `plan`, and the same `POOL_KEY`. This step is
synchronous by design — the spammer in Step 7 only starts after `broadcast` returns; no extra sleep/poll
needed in the script.)

**Step 7 — start the spammer** (replaces `blueprint run jettonSpam` / `jettonSpamLs.ts`):
```bash
SPAM_PID_FILE="$JETTON_BOMB_DIR/jetton-spam.pid"
SPAM_LOG="$JETTON_BOMB_DIR/jetton-spam.$(date '+%Y%m%d-%H%M%S').log"
POOL_KEY_FILE="$POOL_KEY" nohup "$SPAM_BIN" --mode "$SPAM_MODE" "$NUM_WALLETS" "$FREQ" \
  --endpoint "$DEVNET_ENDPOINT" --minter "$DEVNET_MINTER" --init-mode 1 \
  </dev/null > "$SPAM_LOG" 2>&1 &
echo $! > "$SPAM_PID_FILE"
echo "$JETTON_STEP_PREFIX spam pid=$(cat "$SPAM_PID_FILE") log=$SPAM_LOG"
```
(`--init-mode 1` = bare/cheapest, correct because recipients are pre-deployed. `--mode external` for
`externals-spam`; `--value` overrides the attached GRAM if needed.)

**Step 8 — stop** (`jetton-spam-stop` / `externals-spam-stop`): update `stop_jetton_spam_global` to match
the Go process instead of blueprint:
```bash
pkill -f "$JETTON_BOMB_DIR/go-spam/build/spam" 2>/dev/null || pkill -f 'go-spam/build/spam' 2>/dev/null
```
(SIGTERM → the spammer prints a clean final summary and exits; also remove the PID file.)

### Legacy → new command mapping

| Legacy step | New |
|---|---|
| force `blueprint.config.ts` endpoint | pass `--endpoint http://devnet-01.toncenter.com` to `deploy`/`spam` |
| node22 + `npm install` + blueprint upgrade | `make -C "$JETTON_BOMB_DIR"` (Go build) |
| derive+fund v4r2 deployer, `NUM_WALLETS*1005 TON` | fund **wallet0** with `DEPLOY_FUNDING_NANO` (`(count+1)*100.1 GRAM` at the devnet default) via the same `topup_wallet_from_main_wallet` |
| `blueprint run deployWalletSpam` (marker `Sent reproduce call`) | `deploy plan` → fund → `deploy broadcast` (marker `DEPLOY DONE`) |
| read `mnemonic.txt` | `jetton-prepaid/pool.key` (raw seed, generated by `deploy plan`); `spam` reads the same file |
| `blueprint run jettonSpam` | `spam --mode jetton "$NUM_WALLETS" "$FREQ" --endpoint … --minter … --init-mode 1` |
| `ts-node jettonSpamLs.ts` (ONLY_EXT) | `spam --mode external "$NUM_WALLETS" "$FREQ" --endpoint … --minter …` |
| stop: `pkill … jettonSpam` | `pkill -f go-spam/build/spam` |

### Notes / gotchas
- **v3 not required.** Everything uses toncenter **v2** (`/sendBoc`, `/runGetMethod`,
  `/getAddressInformation`). No traces/waitForTrace, no acton, no v3.
- **Re-runs.** With Step 3 (`rm -f pool.key`) every run is a fresh pool — clean, matches legacy `NEW=1`,
  and safe after `ton drop` (new chain). Without it, `deploy broadcast` **refuses** (exits non-zero) when
  wallet0 is already deployed — the kickoff is single-use (seqno 0) and cannot re-run, so it errors
  rather than false-reporting. To re-deploy, delete `pool.key` (fresh pool) or wipe the chain.
- **`deploy broadcast` verification is two-stage.** First it polls wallet0's seqno up to ~120s (a fast
  fail if wallet0 was underfunded or the endpoint is wrong — the kickoff BoC may still have been accepted;
  check wallet0 on-chain). Then it waits for the **terminal wallet** (subwallet `count`) — the end of the
  reproduce chain — the causally-last wallet, so `DEPLOY DONE` means the WHOLE pool is up. A `--pool-timeout`
  (default 10m) expiry means the terminal never deployed — almost always the tail **starved on fees**: raise
  `FEE_SLACK` (the rolling cushion; real cost ~0.02/wallet, default 0.1). It exits non-zero so the script
  won't spam a half-built pool.
- **count must match** across `plan`, `broadcast`, and `spam` (it defines the wallet-id range).

---

## 6. This session's concrete changes (changelog for reviewers)

- **Native Go deploy** (`go-spam/cmd/deploy` + `pkg/deploy`): stands up the whole pool with no acton, no
  minter contract, no deployer wallet — fund wallet0, then one pool-key external (with wallet0
  state_init) deploys wallet0 + fires the reproduce chain. Kickoff external is **byte-identical** to the
  acton `buildKickoffExternal` (`pkg/deploy/parity_test.go` vs `scripts/_deploy_parity.tolk`);
  `plan` auto-generates `pool.key`.
- **Terminal-wallet readiness wait + progress bar** (`cmd/deploy` `waitReady`/`probeSamples`/`progressBar`,
  addresses from `pkg/deploy` `ReadinessSamples`): `broadcast` waits for subwallet `count` — the causally-LAST
  wallet in the reproduce chain — before `DEPLOY DONE`, so the whole pool is up before the spammer starts (no
  lingering failed%). Each poll it samples ~20 subwallets across `[0,count]` and draws a `\r`-redrawn progress
  bar so the multi-minute wait shows movement; the terminal (last sample) is the completion signal, the rest
  are UX. `--pool-timeout`/`--poll-interval`; timeout → non-zero exit. Unit-tested (`cmd/deploy/main_test.go`);
  the `ton` script needs **no** change.
- **Fee-cushion fix** (`FEE_SLACK` default 0.01 → **0.1** in `cmd/deploy` for devnet): the rolling per-wallet
  fee slack was below the real **devnet** ~0.02/wallet cascade cost, so the bank slowly starved and the last
  driver (~1 share short) failed to deploy the tail — confirmed on a live 10k devnet pool via toncenter
  (deployed to id ~9801, which sat holding ~19,900 GRAM having failed its fan-out). **Mainnet cascade cost is
  ~20x lower (~0.001/wallet)** — a live 5k mainnet pool at 0.01 deployed fully with ~43/50 GRAM slack unused —
  so the acton `deploy.tolk` mainnet default is a modest 0.01 → **0.015** (don't apply the devnet 0.1 there).
- **Backend `SendExternalMessageWithInit`** (`pkg/backend`): send an external carrying the destination's
  state_init (LS via `tlb.ExternalMessage.StateInit`; HTTP via an init ref in the ext-msg BoC).
- **Headless dual-mode spammer** (`go-spam/cmd/spam`): `--mode jetton` (real transfers) / `--mode
  external`; jetton external builder shared with the bot in `pkg/jetton` (byte-identical, golden-tested).
- **Root `Makefile`**: `make` builds `deploy`, `spam`, `bot` into `go-spam/build/`.
- **Deploy idempotency** (acton `common.tolk`): re-running the acton deploy skips an already-deployed
  minter (dev-time convenience; not used on devnet).
- **Go bot audit log** (`cmd/bot/audit.go`): optional `AUDIT_CHAT_ID` private-channel audit (bot only).

## 7. Cost reference (mainnet-basechain; devnet gas config may differ — re-measure)

| stage | fees/transfer | drain/transfer |
|---|---|---|
| Legacy FunC (state_init on transfer) | 0.00458 | ~0.0056 |
| Tolk+prepaid, pool recipient, init-mode 0 | 0.001483 | 0.002068 |
| Tolk+prepaid, pool recipient, init-mode 1 (bare) | 0.001324 | 0.001544 |

At 300 transfers/s a 5000-wallet pool (~0.19 GRAM/wallet) lasts ~25 min (mode 0) / ~34 min (mode 1).

---

## 8. Key files

- Go deploy: `go-spam/cmd/deploy`, `go-spam/pkg/deploy` (+ `parity_test.go`); backend init send in `go-spam/pkg/backend/{backend,ls,http}.go`
- Go load: `go-spam/cmd/spam`, `go-spam/pkg/jetton` (+ golden test), `go-spam/pkg/{pool,walletspam}`
- Build: root `Makefile` (`make` → `go-spam/build/{deploy,spam,bot}`)
- Pool key: `jetton-prepaid/pool.key` (raw 32B ed25519 seed, gitignored; generated by `deploy plan`)
- Contracts (dev-time, acton): `jetton-prepaid/contracts/*.tolk`; parity vector `jetton-prepaid/scripts/_deploy_parity.tolk`
- Legacy (still present): `scripts/{deployWalletSpam,jettonSpam,jettonSpamLs}.ts`
- Repo guide: `CLAUDE.md`
