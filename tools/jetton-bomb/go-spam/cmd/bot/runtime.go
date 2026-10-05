package main

import (
	"context"
	"crypto/ed25519"
	"encoding/json"
	"fmt"
	"log"
	mathrand "math/rand"
	"os"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	tg "github.com/go-telegram/bot"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
	"github.com/victor/jetton-bomb-go-spam/pkg/gram"
	"github.com/victor/jetton-bomb-go-spam/pkg/highload"
	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
	"github.com/victor/jetton-bomb-go-spam/pkg/streaming"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletv5"
)

// DefaultSubPoolSize is the number of wallets one run drives when POOL_SIZE is unset. 100k matches the
// legacy pool on purpose: a default run touches the same number of contracts as before the sub-pool
// scheme, so the load profile (and therefore the TPS) stays comparable. Live: /pool_size, POOL_SIZE env.
//
// It is only a default. On a small deployment (say a 5k mainnet pool) it is CLAMPED to the deployed
// total — neither this nor DefaultPoolTotal is treated as ground truth anywhere.
const DefaultSubPoolSize = 100000

// DefaultPoolTotal is the deployed WalletSpam pool size assumed when POOL_TOTAL is unset — the 2.5M
// testnet pool. Set POOL_TOTAL (alias: HAMSTER_POOL_SIZE) to whatever you actually deployed: the bot
// drives [0, poolTotal) and would otherwise address subwallet ids that were never stood up on-chain.
// Runs take a WINDOW of it (see Runtime.pickSubPool); /hamster_kombat and the fat modes take all of it.
const DefaultPoolTotal = 2500000

// SpamMinterTON is the SPAM_MINTER from scripts/lib/addresses.ts (MINTERS.pub4).
// const SpamMinterTON = "EQBECJWGkqPCLTugqIFQPK_s6NU-nfzkLG0mxdiu2hdCxwdw" // 100 default balance
const SpamMinterTON = "kQCy3cWuUXredDmXUTipDo31Juda2Y10jfK9DYS0gBPAjD-X" // 1m default balance

const (
	RunAmountPerShardTON = "1000"
	StopFeePerShardTON   = "0.05"
)

// introMessage is shown on /start without an argument — a quick command cheatsheet.
const introMessage = `TPS spam bot. /help shows this again, /status shows only the status block.

Load (one pool mode at a time):
/jetton <freq> — pool wallets send jettons to random pool wallets
/hamster_kombat <freq> — same, but over the WHOLE deployed pool (ignores /pool_size)
/gram <freq> — pool wallets send ordinary GRAM transfers
/gassless_jetton <freq> — jetton transfers batched by the 4 highload relayers (they pay the gas)
/gassless_gram <freq> — GRAM transfers relayed the same way
/fat_gram <freq> — GRAM transfers to big-state fat accounts
/fat_jetton <freq> — jetton transfers to big-state fat accounts (notification lands on the fat)
/external <freq> — pool wallets receive empty externals (seqno bump only)
/start <tps> — 1 contract spams itself
/shards <tps> — 16 contracts spam themselves (each with a different address prefix)

/stop — STOP EVERYTHING at once (any pool mode + both static runs), reports what it stopped

Stop one thing (hidden from the command menu, still work):
/jetton_stop, /hamster_kombat_stop, /gram_stop, /gassless_jetton_stop, /gassless_gram_stop,
/fat_gram_stop, /fat_jetton_stop, /external_stop, /static_stop, /shards_stop

Backend:
/api [url] [key] — switch to HTTP (toncenter)
/config — upload liteserver config .json
/timeout <ms> — per-send timeout (default 1500)

Tuning:
/initmode 0|1 — jetton transfer init mode: 0 = full state_init, 1 = bare (default 0)
/pool_size <n> — wallets driven per run (default 100000); the pool is cut into fixed
  sub-pools of n and each run picks one at random, so it wears evenly
/fat_size 10|25|50|100|200 — size of the fat accounts targeted (KB, default 100)
/fat_count <n> — how many fat accounts a run aims at (default all of the tier; /fat_size resets it)
/fat_mutate on|off — /fat_jetton changes the fat's storage, or only reads it (default off, live)
/errors on|off — append error breakdown to status messages (default on)
/retry on|off — refetch+resend on a failed send (default on)
/check_balance on|off [nano] — skip drained wallets, auto-stop at 75% drained (default off)
/streaming on|off — sample confirmed transfer delivery (requires streaming API key)`

// buildIntroMessage returns the cheatsheet plus a status block: backend, timeout,
// and any tracked loops. /start and /shards state reflects only commands issued
// by this bot instance — restart loses the flag (the on-chain contract keeps
// running but we no longer report it).
func (r *Runtime) buildIntroMessage() string {
	return introMessage + "\n\n" + r.buildStatusBlock()
}

// buildStatusBlock is the status half of /help on its own — what /status prints, so an operator
// mid-run can check backend/loops/relayers without scrolling past the cheatsheet every time.
func (r *Runtime) buildStatusBlock() string {
	var sb strings.Builder
	sb.WriteString("Status:\n")

	backendDesc := "(not configured)"
	if be := r.Backend(); be != nil {
		backendDesc = be.Description()
	}
	fmt.Fprintf(&sb, "Backend: %s\n", backendDesc)
	fmt.Fprintf(&sb, "Timeout: %s\n", r.SendTimeout())
	errState := "off"
	if r.ErrorsEnabled() {
		errState = "on"
	}
	fmt.Fprintf(&sb, "Errors breakdown: %s\n", errState)
	retryState := "on"
	if r.noRetry.Load() {
		retryState = "off"
	}
	fmt.Fprintf(&sb, "Send retry: %s\n", retryState)
	streamState := "not configured"
	if r.stream != nil {
		streamState = "off"
		if r.stream.Enabled() {
			streamState = "connecting"
			if r.stream.Ready() {
				streamState = "on"
			}
		}
		streamState += fmt.Sprintf(" (%d/%d addresses, 90s window)", r.stream.AddressCount(), r.stream.Limit())
	}
	fmt.Fprintf(&sb, "Streaming: %s\n", streamState)

	fmt.Fprintf(&sb, "Pool: %s wallets · %s per run (%d sub-pools)\n",
		groupDigits(r.poolTotal), groupDigits(r.SubPoolSize()), r.poolTotal/r.SubPoolSize())
	sb.WriteString("Pool spam: ")
	writeSpamRunStatus(&sb, r.poolRun.Load())
	if run := r.streamLast.Load(); run != nil && r.streamMemory != nil {
		s := r.streamMemory.Snapshot(run.startedAt.UnixNano())
		fmt.Fprintf(&sb, "\nDelivery (%s): confirmed %d · pending %d · >90s %d · interrupted %d · send failed %d · no slots %d · p50 %s · p95 %s · p99 %s",
			run.kind, s.Confirmed, r.stream.Pending(run.startedAt.UnixNano()), s.Missing, s.Interrupted, s.SendFailed, s.NoSlot,
			formatLatency(s.P50), formatLatency(s.P95), formatLatency(s.P99))
		fmt.Fprintf(&sb, "\nFinalized (%s): %d · >90s %d · p50 %s · p80 %s · p95 %s · p99 %s",
			run.kind, s.Finalized, s.FinalizedMissing,
			formatLatency(s.FinalizedP50), formatLatency(s.FinalizedP80),
			formatLatency(s.FinalizedP95), formatLatency(s.FinalizedP99))
	}
	sb.WriteString("\nStatic (1 shard): ")
	writeStaticRunStatus(&sb, r.staticRun.Load())
	sb.WriteString("\nStatic (16 shards): ")
	writeStaticRunStatus(&sb, r.shardsRun.Load())

	// Relayer funding, from the last snapshot the gasless watcher published (no RPC here — /help must stay
	// instant). Absent until the first gasless run; `highload status` reads it live from the CLI.
	if snap := r.hlBal.Load(); snap != nil {
		low := "all funded"
		if len(snap.low) > 0 || snap.errs > 0 {
			low = fmt.Sprintf("%d low, %d unreadable", len(snap.low), snap.errs)
		}
		fmt.Fprintf(&sb, "Relayers (%d highload): %s · min %s TON · %.0fs ago\n", r.highloadCount,
			low, fmtTONNano(snap.min()), time.Since(snap.at).Seconds())
	} else {
		fmt.Fprintf(&sb, "Relayers (%d highload): not checked yet\n", r.highloadCount)
	}

	r.spamPoolMu.Lock()
	p := r.spamPool
	r.spamPoolMu.Unlock()
	if p != nil {
		cs := p.CacheStats()
		pct := 0.0
		if cs.Total > 0 {
			pct = float64(cs.SeqnoCached) / float64(cs.Total) * 100
		}
		fmt.Fprintf(&sb, "\nPool cache: %d/%d warm (%.1f%%) · touched %d",
			cs.SeqnoCached, cs.Total, pct, cs.Touched)
	} else {
		sb.WriteString("\nPool cache: not initialized")
	}

	return sb.String()
}

func writeSpamRunStatus(sb *strings.Builder, run *SpamRun) {
	if run == nil {
		sb.WriteString("idle")
		return
	}
	elapsed := time.Since(run.startedAt).Seconds()
	sent := run.stats.Sent.Load()
	avg := 0.0
	if elapsed > 0 {
		avg = float64(sent) / elapsed
	}
	fmt.Fprintf(sb, "%s · %d req/s · %.1fs · sent %d (avg %.1f/s)", run.kind, run.freq, elapsed, sent, avg)
}

func writeStaticRunStatus(sb *strings.Builder, run *StaticRun) {
	if run == nil {
		sb.WriteString("idle")
		return
	}
	elapsed := time.Since(run.startedAt).Seconds()
	if run.shards > 1 {
		fmt.Fprintf(sb, "running %d TPS/shard (~%d total) · %.1fs",
			run.tps, run.tps*run.shards, elapsed)
	} else {
		fmt.Fprintf(sb, "running %d TPS · %.1fs", run.tps, elapsed)
	}
}

// Runtime holds bot-wide state. Backend is swappable at runtime via /api and /config.
type Runtime struct {
	backend       atomic.Pointer[backend.Backend] // pointer-to-interface for atomic swap
	wallet        *walletv5.Wallet                // V5R1 control wallet (WALLET_MNEMONIC)
	shardAddrs    []*address.Address              // 16 StaticTps addresses
	spamPub       ed25519.PublicKey               // pool keypair (mnemonic.txt)
	spamPriv      ed25519.PrivateKey
	spamCode      *cell.Cell
	poolTotal     int       // deployed pool size [0, poolTotal); POOL_TOTAL env, default PoolTotal
	jettonValue   tlb.Coins // GRAM attached to each op::transfer; JETTON_TRANSFER_VALUE env, default 0.05
	gramValue     tlb.Coins // native value moved by /gram and /gassless_gram; GRAM_TRANSFER_VALUE
	gasslessValue tlb.Coins // GRAM attached to jetton `sint` requests (default 0.012)
	// Native transfers need only their value plus 0.001 GRAM of target-side fee headroom.
	gasslessGramValue tlb.Coins // GASSLESS_GRAM_RELAY_VALUE; default gramValue + 0.001
	// gasslessJettonValue is the GRAM the target attaches to its own op::transfer in /gassless_jetton
	// (default 0.01; the relayer's 0.012 covers it plus ~0.002 of target-side fees). Separate from
	// jettonValue so /jetton and /hamster_kombat keep their own funding level.
	gasslessJettonValue tlb.Coins
	jettonMinter        *address.Address
	jwCode              *cell.Cell           // JettonWallet code; part of each WalletSpam's storage, so it fixes the pool address
	defaultHTTP         *backend.HTTPBackend // base HTTP from env, used by /api with no args
	defaultLSPath       string               // env LS_CONFIG, used at startup if HTTP not set

	// All pool-backed modes share one active slot. This prevents a direct external racing a target
	// seqno that a gasless pair has reserved but not yet executed on-chain.
	poolRun      atomic.Pointer[SpamRun]
	stream       *streaming.Monitor
	streamMemory *streaming.Memory
	streamLast   atomic.Pointer[SpamRun]

	// StaticTps tracking. The bot just fires a run/stop message at the on-chain
	// contract; these flags only reflect what *we* last told it. May drift across
	// bot restarts or out-of-band actions, but it's enough for /start status.
	staticRun atomic.Pointer[StaticRun] // /start (1 shard)
	shardsRun atomic.Pointer[StaticRun] // /shards (16 shards)

	// The single WalletSpam pool (poolTotal wallets, lazily built). Runs drive a sub-pool WINDOW of it.
	spamPool   *pool.Pool
	spamPoolMu sync.Mutex
	// subPoolSize is how many wallets one run drives (/pool_size, POOL_SIZE env, default 100k). The pool is
	// sliced into floor(poolTotal/subPoolSize) fixed sub-pools and each run picks one at random — see
	// pickSubPool for why fixed slices rather than a random offset.
	subPoolSize atomic.Int64

	// Highload-V3 relayers backing the gasless modes: four wallets, one per top-two-bit shard bucket,
	// derived from the POOL key + highloadTTL (so they must match what `highload deploy` used, or the
	// addresses differ). Ground lazily on first use — grinding walks subwallet ids until all 16 buckets
	// are hit, so it's done once and cached. The bot never deploys or tops them up; it only warns below
	// highloadMinBalance.
	highloads     []*highload.HighloadWallet
	highloadsOK   bool
	hlMu          sync.Mutex
	highloadTTL   uint32
	highloadCount int              // HIGHLOAD_SHARDS: four (mainnet default) or 16 (testnet experiment)
	batchLimiter  highload.Limiter // per-address pacing survives stop/start and backend switches
	// highloadMinBalance is the warn threshold in nanoTON: a relayer below it is flagged on the gasless
	// status tick and audited once. No auto-topup — funding is a manual `highload deploy` / transfer.
	highloadMinBalance atomic.Int64
	// hlBal is the latest relayer balance snapshot, published by the watcher during a gasless run and read
	// by the status tick and /help. nil until the first gasless run takes one.
	hlBal atomic.Pointer[hlBalances]

	// Fat load targets (/fat_gram, /fat_jetton): big-state accounts whose storage dict the collator must
	// walk. fatSize is the selected tier in KB (/fat_size, default DefaultFatSizeKB); fatAddrs caches the
	// DERIVED address list per tier (see pkg/fatpool — nothing is stored on disk).
	fatSize  atomic.Int32
	fatAddrs map[int][]*address.Address
	fatMu    sync.Mutex
	// fatCount is how many fat accounts one run aims at (/fat_count). 0 = the whole tier, which is the
	// default and what /fat_size resets it to — a count only means something against a specific tier.
	// Below the tier size, the tier is cut into fixed slices exactly like the sender pool (see
	// pickFatWindow) and each run draws one.
	fatCount atomic.Int64
	// fatJettonValue is the GRAM attached to each /fat_jetton op::transfer; FAT_JETTON_VALUE env, default
	// 0.05. Separate from jettonValue because a fat transfer pays fatForwardTON out of it and gets no
	// excesses back, so the /jetton level (tuned for a refunding pool-to-pool hop) starves it.
	fatJettonValue tlb.Coins
	// fatForwardTON (nanoTON) is forwarded so a transfer_notification reaches the fat itself. MUST be > 0
	// — with 0 only the fat's jetton wallet is touched and its big state is never read.
	fatForwardTON uint64
	// fatMutate sets the fat's mutate flag in the forward payload: touch AND change its storage, vs just
	// reading it (the A/B knob behind cmd/spam's --mutate). Atomic and read per send, so /fat_mutate
	// takes effect MID-RUN — unlike /fat_size, whose recipient list is captured when the run starts.
	fatMutate atomic.Bool

	pendingCfg sync.Map // userID(int64) -> struct{}{}

	timeoutMs atomic.Int64 // per-send context timeout; tuned via /timeout NUMBER

	// jettonInitMode is the internal_init_mode carried in each jetton transfer's customPayload
	// (see JettonWallet.tolk AskToTransfer): 0 = full state_init (classic, deploys the recipient),
	// 1 = bare (no state_init — cheaper, valid because our pool's jetton wallets are pre-deployed).
	// Default 0; tuned via JETTON_INIT_MODE / /initmode.
	jettonInitMode atomic.Int32

	errorsEnabled atomic.Bool // /errors on|off — controls appending breakdown to status messages

	// noRetry gates the pool's refetch+resend on a failed send (LoopConfig.NoRetry). true => single
	// attempt (pre-"Retries" behavior). Default false (retries on); NO_RETRY env / /retry off flip it.
	// Read live by running loops via r.noRetry.Load, so /retry takes effect mid-run.
	noRetry atomic.Bool

	// checkBalance gates jetton sends on the sender wallet's on-chain GRAM balance (LoopConfig.Gate).
	// When on, a wallet below minSendBalance is skipped (Stats.Skipped, never Sent/Failed) and marked
	// drained; once 75% of the pool is drained the /jetton run auto-stops. Default off (mainnet grams
	// cost money, so this is opt-in); CHECK_BALANCE env / /check_balance on|off flip it live.
	checkBalance atomic.Bool
	// minSendBalance is the /check_balance threshold in nanoTON: below it a wallet can't fund a transfer
	// (attached value + gas/fwd) so we skip it. Default jettonValue + 0.01 GRAM; MIN_SEND_BALANCE env /
	// /check_balance's optional 2nd arg override it. Read live in the gate.
	minSendBalance atomic.Int64

	botToken string // Telegram bot token; needed for the manual sendRichMessage HTTP call (go-telegram/bot v1.20 doesn't wrap it yet)

	// Audit log (optional). bot is captured after tg.New; audit() posts one-off who-did-what events
	// (run starts with params, stops with final stats, backend/config changes) to auditChatID — a
	// private channel set via AUDIT_CHAT_ID. Disabled when auditChatID == 0. It NEVER receives the
	// periodic TPS status ticks (those go only to the originating chat via sendStatus).
	bot         *tg.Bot
	auditChatID int64

	logger *log.Logger
	errLog *log.Logger
}

// ErrorsEnabled reports whether /errors is on. Default is true (see main.go init).
func (r *Runtime) ErrorsEnabled() bool { return r.errorsEnabled.Load() }

// SetErrorsEnabled flips the /errors toggle.
func (r *Runtime) SetErrorsEnabled(v bool) { r.errorsEnabled.Store(v) }

// SendTimeout returns the current per-request timeout. Used by spam loops as a
// per-call provider so /timeout takes effect mid-flight.
func (r *Runtime) SendTimeout() time.Duration {
	ms := r.timeoutMs.Load()
	if ms <= 0 {
		ms = 1500
	}
	return time.Duration(ms) * time.Millisecond
}

func (r *Runtime) relayValue() tlb.Coins {
	if r.gasslessValue.Nano().Sign() > 0 {
		return r.gasslessValue
	}
	return tlb.FromNanoTONU(12_000_000) // 0.012 = target's 0.01 + ~0.002 of its fees
}

func (r *Runtime) gramRelayValue() tlb.Coins {
	if r.gasslessGramValue.Nano().Sign() > 0 {
		return r.gasslessGramValue
	}
	return gram.DefaultRelayValue(r.gramValue)
}

// SpamRun captures state for one pool-backed spam loop.
type SpamRun struct {
	cancel    context.CancelFunc
	stats     *pool.Stats
	freq      int
	period    time.Duration
	startedAt time.Time
	kind      string
	errs      *ErrStack

	// Audit live message: the run's audit-chat message is edited in place every status tick with a
	// fresh auditStatLine, so the audit chat shows live stats without a stream of new messages.
	// auditMsgID == 0 when the audit log is disabled. auditHeader is the static top of that message.
	auditMsgID  int
	auditHeader string
}

// StaticRun is the last on-chain TPS run we issued. tps is per-shard.
type StaticRun struct {
	tps       int
	shards    int
	startedAt time.Time
}

func (r *Runtime) Backend() backend.Backend {
	if p := r.backend.Load(); p != nil {
		return *p
	}
	return nil
}

func (r *Runtime) SetBackend(b backend.Backend) {
	r.backend.Store(&b)
}

// loadShardAddresses parses tps_shards.json (16 entries, sorted by .shard).
func loadShardAddresses(path string) ([]*address.Address, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read %s: %w", path, err)
	}
	var data struct {
		Shards []struct {
			Shard   int    `json:"shard"`
			Address string `json:"address"`
		} `json:"shards"`
	}
	if err := json.Unmarshal(raw, &data); err != nil {
		return nil, fmt.Errorf("parse %s: %w", path, err)
	}
	if len(data.Shards) != 16 {
		return nil, fmt.Errorf("expected 16 shards, got %d", len(data.Shards))
	}
	sort.Slice(data.Shards, func(i, j int) bool { return data.Shards[i].Shard < data.Shards[j].Shard })
	out := make([]*address.Address, 16)
	for i, s := range data.Shards {
		a, err := address.ParseAddr(s.Address)
		if err != nil {
			return nil, fmt.Errorf("shard[%d] address %q: %w", i, s.Address, err)
		}
		out[i] = a
	}
	return out, nil
}

// initSpamPool builds (once) the single 2.5M WalletSpam pool every pool-backed mode drives. Entries are
// created lazily inside it, and the cache survives across runs — so consecutive runs over the same
// sub-pool reuse warm seqnos.
func (r *Runtime) initSpamPool() *pool.Pool {
	r.spamPoolMu.Lock()
	defer r.spamPoolMu.Unlock()
	if r.spamPool != nil {
		return r.spamPool
	}
	r.spamPool = pool.New(r.poolTotal, r.spamPub, r.spamPriv, r.jettonMinter, r.jwCode, r.spamCode)
	return r.spamPool
}

// SubPoolSize returns the configured wallets-per-run.
func (r *Runtime) SubPoolSize() int {
	n := int(r.subPoolSize.Load())
	if n <= 0 {
		n = DefaultSubPoolSize
	}
	if n > r.poolTotal {
		n = r.poolTotal
	}
	return n
}

// pickSubPool chooses the contiguous wallet range a run will drive: the pool is cut into
// floor(poolTotal/size) FIXED slices and one is drawn uniformly.
//
// Fixed slices, not a random offset: with a random start every wallet in the middle of the pool would be
// reachable by ~size different windows while the first and last wallets are reachable by only one, so the
// ends would be spent far more slowly than the middle. Cutting the pool into aligned slices gives every
// wallet exactly one slice and every slice the same chance. The tail remainder (poolTotal % size) is
// simply never driven — deliberately, since a short final slice would get the same odds as a full one and
// so be over-spent per wallet.
func (r *Runtime) pickSubPool() (start, size int) {
	size = r.SubPoolSize()
	slices := r.poolTotal / size
	if slices <= 1 {
		return 0, size
	}
	return mathrand.Intn(slices) * size, size
}

// FatCount returns how many fat accounts a run aims at within a tier of `total` accounts. 0 (unset) and
// anything past the tier both mean "all of it", so a /fat_count left over from a bigger tier can never
// out-run a smaller one.
func (r *Runtime) FatCount(total int) int {
	n := int(r.fatCount.Load())
	if n <= 0 || n > total {
		return total
	}
	return n
}

// pickFatWindow chooses the slice of the fat tier a run will aim at — the same fixed-slice scheme
// pickSubPool uses on the sender pool: the tier is cut into floor(total/count) aligned slices and one is
// drawn uniformly, so every fat account belongs to exactly one slice and all slices are equally likely.
// The tail remainder (total % count) is never targeted, deliberately: a short slice drawn as often as a
// full one would over-touch its accounts.
//
// The chosen range is NOT reported in chat — only the count matters to the operator — but it is logged.
func (r *Runtime) pickFatWindow(total int) (start, count int) {
	if total <= 0 {
		return 0, 0
	}
	count = r.FatCount(total)
	slices := total / count
	if slices <= 1 {
		return 0, count
	}
	return mathrand.Intn(slices) * count, count
}

// initHighloads grinds (once) and returns the configured relayers, keyed by shard bucket. Deterministic in
// (pool pubkey, highloadTTL) — the same inputs `highload deploy` used, so the bot addresses exactly the
// relayers that were deployed and funded. The four retained old nibble buckets 0/4/8/c serve full quarters.
func (r *Runtime) initHighloads() []*highload.HighloadWallet {
	r.hlMu.Lock()
	defer r.hlMu.Unlock()
	if !r.highloadsOK {
		r.highloads = highload.NewShardWallets(r.spamPub, r.spamPriv, r.highloadTTL, r.highloadCount)
		r.highloadsOK = true
	}
	return r.highloads
}
