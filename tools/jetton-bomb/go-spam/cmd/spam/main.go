// Headless WalletSpam load generator — the non-Telegram sibling of cmd/bot, meant to be the primary
// binary driven from the devnet orchestration. Spam modes (like the bot's /jetton and /external):
//
// The mode name fully determines both sender and recipient. A "-fats" suffix redirects the recipient from
// "another wallet of the same kind" to a random "fat" load-target account (born in the bench zerostate,
// big storage dict); a real op::transfer with forward_ton>0 lands a transfer_notification ON the fat so the
// collator must walk its dict (the long_collation split axis). Only the two jetton-transfer senders
// (jetton / zerostate) support "-fats".
//
//   --mode jetton         (default) each pool wallet signs an external carrying one op::transfer of 1 jetton
//                         from its OWN pre-deployed jetton wallet to a random OTHER pool wallet. Same builder
//                         as cmd/bot /jetton (pkg/jetton), so externals are byte-identical.
//   --mode jetton-fats    like jetton, but the recipient is a random FAT account from --fats-file (with
//                         forward_ton>0). Uses the pool's own key/minter — needs NO --manifest. The fat's
//                         jetton wallet auto-deploys under the pool minter (init mode 0).
//   --mode external       each pool wallet signs an empty external (seqno bump only) — pure external-
//                         acceptance load, mirrors scripts/jettonSpamLs.ts ONLY_EXT mode.
//   --mode gram           ordinary native GRAM transfers (with a random text comment) between random pool
//                         wallets.
//   --mode gassless-gram / gassless-jetton
//                         aliases of the highload-relayed gasless modes below.
//   --mode highload-jetton / highload-gram
//                         gasless relayed by four dedicated Highload-V3 relayers (one per basechain shard,
//                         chosen by the TARGET's shard so the relay hop is intra-shard), derived from the
//                         POOL key. Deploy them first with cmd/highload. Relayer pays the gas.
//                         Always batched through self-messages: 500ms windows, max 254 messages / 60 KiB.
//   --mode targets        each pool wallet pokes a RANDOM account from --targets (a file of raw wc:hex addrs)
//                         with one bounceable 1-nanogram internal + empty body. Nets ~0 GRAM (it bounces), but
//                         collating each poke forces the recipient's cells out of CellDB — used to test
//                         whether touching the largest-state accounts (see ton-index-go/cmd/bigstate) slows
//                         collation.
//   --mode zerostate      the "ordinary" jetton throughput load: each deterministically-generated wallet-v5
//                         wallet sends a jetton transfer to a random OTHER generated wallet-v5 wallet (both
//                         born in the bench zerostate, no forward). Recipients are pre-deployed, so
//                         --init-mode 1 (bare) is the cheapest correct option, but the default is 0 (full
//                         state_init). Reads the seed / minter / wallet count from the state-gen
//                         manifest.json; no fats.addrs needed.
//   --mode zerostate-fats like zerostate (generated wallet-v5 sender), but the recipient is a random FAT
//                         account from fats.addrs (with forward_ton>0, init mode 0). Needs --manifest +
//                         --fats-file. Supersedes the old --mode fats.
//
// Both CLI flags and env variables are supported. Priority: explicit flag > positional arg > env > default.
//
// Examples:
//   spam 100000 400                                   # jetton mode (default), positional count freq
//   spam --mode external 100000 400                   # external-acceptance load
//   ONLY_EXT=1 spam 100000 400                        # same (env alias for --mode external)
//   spam -c 100000 -f 400 -e http://devnet-01.toncenter.com --minter EQ... --init-mode 1
//   COUNT=100000 FREQ=400 spam                        # legacy env
//
// Flags / env:
//   -c, --count            COUNT                 required, number of subwallet ids
//   -f, --freq             FREQ                  sends per second (XOR with --period)
//   -p, --period           PERIOD                ms between fires
//       --mode             MODE                  jetton | jetton-fats | gram | gassless-jetton | gassless-gram | highload-jetton | highload-gram | external | targets | zerostate | zerostate-fats (default jetton; ONLY_EXT=1 => external)
//       --value            JETTON_TRANSFER_VALUE GRAM attached per transfer, jetton mode (default 0.05)
//                          GASSLESS_JETTON_VALUE for gasless jetton modes (default 0.01); --value wins
//       --gram-value       GRAM_TRANSFER_VALUE   native GRAM transferred in gram modes (default 0.001)
//       --relay-value      GASSLESS_RELAY_VALUE  target `sint` funding (GRAM default gram-value + 0.001; jetton 0.012)
//                          GASSLESS_GRAM_RELAY_VALUE overrides the general env for GRAM; --relay-value wins
//       --highload-ttl     HIGHLOAD_TTL          highload modes: query-id/validity window seconds (default 3600; must match deploy)
//       --highload-shards  HIGHLOAD_SHARDS       highload modes: 4 (default) or 16 relayers
//       --init-mode        JETTON_INIT_MODE      recipient internal_init_mode: 0 full state_init | 1 bare
//       --targets          TARGETS_FILE          targets mode: file of raw wc:hex addresses (one per line)
//       --target-nano      TARGET_NANO           targets mode: nanoTON per poke (default 1)
//       --order            TARGETS_ORDER         targets mode: random (default) | sequential (file order)
//       --spam-log         SPAM_LOG              detailed per-send log path, tee'd to stdout (default spam.log)
//       --target-sizes     TARGET_SIZES          targets mode: account\tbytes file for poke-size log (default <targets>.sizes)
//   -l, --ls-config        LS_CONFIG             TON global config path (default ~/genesis/devnet-liteservers-only.json)
//   -e, --endpoint         TONCENTER_ENDPOINT    if set, use HTTP backend (Toncenter v2 root) instead of LS
//   -k, --api-key          TONCENTER_API_KEY     toncenter API key for HTTP
//   -m, --pool-key         POOL_KEY_FILE         pool key file, 32-byte ed25519 seed (auto: jetton-prepaid/pool.key)
//       --minter           WALLET_MINTER         jetton minter address (fixes the pool address; default = SpamMinterTON)
//   -t, --send-timeout     SEND_TIMEOUT          per-call timeout in ms (default 1500)
//       --max-inflight     MAX_INFLIGHT          cap on concurrent goroutines (default 2*FREQ+10)
//       --no-retry         NO_RETRY              single-attempt sends: no refetch+resend on failure
//       --check-balance    CHECK_BALANCE         gate sends on sender balance: skip drained wallets, stop at 75%
//       --min-send-balance MIN_SEND_BALANCE      check-balance threshold in nanoTON (0 = auto, mode-aware)
//       --error-log        ERROR_LOG             error log path (default errors.log)
//   zerostate / zerostate-fats modes (both send FROM the generated wallet-v5 wallets):
//       --manifest         MANIFEST_FILE         state-gen manifest.json (seed / minter / num_v5 / wallet_id)
//       --seed             SEED_HEX              override manifest seed_hex (64 hex chars)
//       --value            JETTON_TRANSFER_VALUE GRAM attached per transfer (also used in zerostate modes)
//   -fats modes (jetton-fats / zerostate-fats) — recipient is a fat load-target account:
//       --fats-file        FATS_FILE             fats.addrs recipients (default <manifest dir>/fats.addrs)
//       --forward-ton      FORWARD_TON           GRAM forwarded so the notification reaches the fat (default 0.01)
//       --mutate           MUTATE                set the fat mutate flag (default off = touch without mutating)
//       --order            TARGETS_ORDER         random (default) | sequential over fats.addrs

package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"math/big"
	mathrand "math/rand"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
	"github.com/victor/jetton-bomb-go-spam/pkg/drain"
	"github.com/victor/jetton-bomb-go-spam/pkg/gram"
	"github.com/victor/jetton-bomb-go-spam/pkg/highload"
	"github.com/victor/jetton-bomb-go-spam/pkg/jetton"
	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
	"github.com/victor/jetton-bomb-go-spam/pkg/sidechain"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

// SpamMinterTON is the default jetton minter (must match the cmd/bot SpamMinterTON so the standalone
// spammer targets the same on-chain pool). The minter address is part of each WalletSpam's storage,
// so it — together with the WalletSpam + JettonWallet code — determines the pool address.
const SpamMinterTON = "kQCy3cWuUXredDmXUTipDo31Juda2Y10jfK9DYS0gBPAjD-X" // 1m default balance

func envStr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func defaultLSConfigPath() string {
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return "./config.json"
	}
	return filepath.Join(home, "genesis", "devnet-liteservers-only.json")
}

func envInt(k string, def int) int {
	if v := os.Getenv(k); v != "" {
		n, err := strconv.Atoi(v)
		if err == nil {
			return n
		}
	}
	return def
}

func envFloat(k string, def float64) float64 {
	if v := os.Getenv(k); v != "" {
		f, err := strconv.ParseFloat(v, 64)
		if err == nil {
			return f
		}
	}
	return def
}

func envTruthy(k string) bool {
	switch strings.ToLower(os.Getenv(k)) {
	case "1", "true", "yes", "on":
		return true
	}
	return false
}

// normalizeMode maps user input to a canonical mode, or "" if unrecognized. The recipient/sender is fully
// determined by the mode name: a "-fats" suffix redirects the recipient to a fat load-target account (only
// "jetton" and "zerostate" senders support it). Canonical: "jetton" / "jetton-fats" / "gram" /
// "gassless-gram" / "gassless-jetton" / "external" / "targets" / "zerostate" / "zerostate-fats".
func normalizeMode(m string) string {
	s := strings.ToLower(strings.TrimSpace(m))
	fatsTarget := strings.HasSuffix(s, "-fats")
	base := strings.TrimSuffix(s, "-fats")
	switch base {
	case "jetton", "jet", "j":
		base = "jetton"
	case "external", "ext", "e":
		base = "external"
	case "gram", "grams", "ton":
		base = "gram"
	case "gassless-gram", "gassless_gram", "gasless-gram", "gasless_gram":
		base = "gassless-gram"
	case "gassless-jetton", "gassless_jetton", "gasless-jetton", "gasless_jetton":
		base = "gassless-jetton"
	case "highload-jetton", "hl-jetton", "highload-gassless-jetton", "hl-gassless-jetton":
		base = "highload-jetton"
	case "highload-gram", "hl-gram", "highload-gassless-gram", "hl-gassless-gram":
		base = "highload-gram"
	case "targets", "target", "big", "bigspam":
		base = "targets"
	case "zerostate", "zero", "zs", "w5", "v5":
		base = "zerostate"
	default:
		return ""
	}
	if fatsTarget {
		// Only the two jetton-transfer senders can aim at a fat; external/targets can't.
		if base != "jetton" && base != "zerostate" {
			return ""
		}
		return base + "-fats"
	}
	return base
}

// loadTargets reads a file of raw wc:hex addresses (one per line, "0:HEX…" / "-1:HEX…"; blank lines and
// leading '#' comments skipped) into a de-duplicated slice. Used by --mode targets to poke big-state
// accounts and force their state out of CellDB.
func loadTargets(path string) ([]*address.Address, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read targets %s: %w", path, err)
	}
	var out []*address.Address
	seen := make(map[string]struct{})
	for i, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		a, err := address.ParseRawAddr(line)
		if err != nil {
			return nil, fmt.Errorf("targets %s line %d: bad raw address %q: %w", path, i+1, line, err)
		}
		if _, dup := seen[line]; dup {
			continue
		}
		seen[line] = struct{}{}
		out = append(out, a)
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no addresses in %s", path)
	}
	return out, nil
}

func die(format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(1)
}

func resolveRelayValue(mode string, gramValue tlb.Coins, override string, explicitFlag bool) (tlb.Coins, error) {
	gramMode := mode == "gassless-gram" || mode == "highload-gram"
	if gramMode && !explicitFlag {
		override = envStr("GASSLESS_GRAM_RELAY_VALUE", envStr("GASLESS_GRAM_RELAY_VALUE", override))
	}
	if override == "" {
		if gramMode {
			return gram.DefaultRelayValue(gramValue), nil
		}
		override = "0.012"
	}
	v, err := tlb.FromTON(override)
	if err != nil || v.Nano().Sign() <= 0 {
		return tlb.Coins{}, fmt.Errorf("invalid relay value %q: expected positive GRAM", override)
	}
	return v, nil
}

func jettonValueForMode(mode, value string, explicitFlag bool) string {
	if !explicitFlag && (mode == "gassless-jetton" || mode == "highload-jetton") {
		return envStr("GASSLESS_JETTON_VALUE", "0.01")
	}
	return value
}

func main() {
	var (
		count         int
		freq          float64
		period        float64
		mode          string
		valueStr      string
		gramValueStr  string
		relayValueStr string
		initMode      int
		targetsPath   string
		targetNano    int64
		orderStr      string
		spamLogPath   string
		targetSizes   string
		configPath    string
		endpoint      string
		apiKey        string
		poolKeyPath   string
		minterStr     string
		sendTimeoutMs int
		maxInflight   int
		errorLogPath  string
		noRetry       bool
		checkBalance  bool
		minSendNano   int64
		// fats mode
		fatsFile      string
		fatsManifest  string
		fatsSeedHex   string
		forwardTONStr string
		fatsMutate    bool
		// highload relay modes (relayers share the POOL key — same pool.key, no separate keyfile)
		highloadTTL    int
		highloadShards int
	)

	// ONLY_EXT=1 (the devnet script's convention) sets the default mode to external; --mode still wins.
	defaultMode := envStr("MODE", "jetton")
	if envTruthy("ONLY_EXT") {
		defaultMode = "external"
	}
	defaultLSConfig := envStr("LS_CONFIG", defaultLSConfigPath())

	// Defaults are seeded from env so unset flags fall back to env.
	flag.IntVar(&count, "count", envInt("COUNT", 0), "number of subwallet ids (required)")
	flag.IntVar(&count, "c", envInt("COUNT", 0), "short for --count")
	flag.Float64Var(&freq, "freq", envFloat("FREQ", 0), "sends per second")
	flag.Float64Var(&freq, "f", envFloat("FREQ", 0), "short for --freq")
	flag.Float64Var(&period, "period", envFloat("PERIOD", 0), "ms between fires (XOR --freq)")
	flag.Float64Var(&period, "p", envFloat("PERIOD", 0), "short for --period")
	flag.StringVar(&mode, "mode", defaultMode, "spam mode: jetton | jetton-fats | gram | gassless-jetton | gassless-gram | external | targets | zerostate | zerostate-fats")
	flag.StringVar(&valueStr, "value", envStr("JETTON_TRANSFER_VALUE", "0.05"), "GRAM attached per transfer (jetton mode)")
	flag.StringVar(&gramValueStr, "gram-value", envStr("GRAM_TRANSFER_VALUE", "0.001"), "native GRAM transferred in gram modes")
	flag.StringVar(&relayValueStr, "relay-value", envStr("GASSLESS_RELAY_VALUE", envStr("GASLESS_RELAY_VALUE", "")), "signed-internal funding (default: gram-value + 0.001 for GRAM, 0.012 for jetton; GASSLESS_GRAM_RELAY_VALUE overrides general env for GRAM)")
	flag.IntVar(&initMode, "init-mode", envInt("JETTON_INIT_MODE", 0), "recipient internal_init_mode: 0 full state_init | 1 bare")
	flag.StringVar(&targetsPath, "targets", envStr("TARGETS_FILE", ""), "targets mode: file of raw wc:hex addresses, one per line (each pool wallet pokes a random one)")
	flag.Int64Var(&targetNano, "target-nano", int64(envInt("TARGET_NANO", 1)), "targets mode: nanoTON attached to each poke (default 1)")
	flag.StringVar(&orderStr, "order", envStr("TARGETS_ORDER", "random"), "targets mode: 'random' (default) or 'sequential' (walk the file in order, biggest-first)")
	flag.StringVar(&spamLogPath, "spam-log", envStr("SPAM_LOG", "spam.log"), "detailed per-send log (targets mode logs every poke here AND to stdout)")
	flag.StringVar(&targetSizes, "target-sizes", envStr("TARGET_SIZES", ""), "targets mode: account\\tbytes file to annotate poke sizes (default <targets>.sizes)")
	flag.StringVar(&configPath, "ls-config", defaultLSConfig, "TON global config path (for LS backend)")
	flag.StringVar(&configPath, "l", defaultLSConfig, "short for --ls-config")
	flag.StringVar(&endpoint, "endpoint", envStr("TONCENTER_ENDPOINT", ""), "if set, use HTTP backend at this Toncenter endpoint")
	flag.StringVar(&endpoint, "e", envStr("TONCENTER_ENDPOINT", ""), "short for --endpoint")
	flag.StringVar(&apiKey, "api-key", envStr("TONCENTER_API_KEY", ""), "Toncenter API key for HTTP backend")
	flag.StringVar(&apiKey, "k", envStr("TONCENTER_API_KEY", ""), "short for --api-key")
	flag.StringVar(&poolKeyPath, "pool-key", envStr("POOL_KEY_FILE", backend.DefaultPoolKeyFile()), "pool key file (raw 32-byte ed25519 seed)")
	flag.StringVar(&poolKeyPath, "m", envStr("POOL_KEY_FILE", backend.DefaultPoolKeyFile()), "short for --pool-key")
	flag.StringVar(&minterStr, "minter", envStr("WALLET_MINTER", SpamMinterTON), "jetton minter address (fixes the pool address)")
	flag.IntVar(&sendTimeoutMs, "send-timeout", envInt("SEND_TIMEOUT", 1500), "per-call timeout in ms")
	flag.IntVar(&sendTimeoutMs, "t", envInt("SEND_TIMEOUT", 1500), "short for --send-timeout")
	flag.IntVar(&maxInflight, "max-inflight", envInt("MAX_INFLIGHT", 0), "cap on concurrent goroutines (0 = 2*rate+10)")
	flag.StringVar(&errorLogPath, "error-log", envStr("ERROR_LOG", "errors.log"), "error log path")
	flag.BoolVar(&noRetry, "no-retry", envTruthy("NO_RETRY"), "single-attempt sends: no refetch+resend on failure (pre-'Retries' behavior)")
	flag.BoolVar(&checkBalance, "check-balance", envTruthy("CHECK_BALANCE"), "gate sends on the sender wallet's balance: skip drained wallets (Skipped, not Failed) and auto-stop at 75% drained")
	flag.Int64Var(&minSendNano, "min-send-balance", int64(envInt("MIN_SEND_BALANCE", 0)), "check-balance threshold in nanoTON (0 = auto: jetton value+0.01 GRAM in jetton mode, else 0.01 GRAM)")
	flag.StringVar(&fatsManifest, "manifest", envStr("MANIFEST_FILE", ""), "zerostate modes: state-gen manifest.json (source of seed / minter / num_v5 / wallet_id)")
	flag.StringVar(&fatsFile, "fats-file", envStr("FATS_FILE", ""), "-fats modes: fats.addrs recipient file (default <manifest dir>/fats.addrs)")
	flag.StringVar(&fatsSeedHex, "seed", envStr("SEED_HEX", ""), "zerostate modes: override manifest seed_hex (64 hex chars)")
	flag.StringVar(&forwardTONStr, "forward-ton", envStr("FORWARD_TON", "0.01"), "-fats modes: GRAM forwarded so a transfer_notification reaches the fat (must be > 0)")
	flag.BoolVar(&fatsMutate, "mutate", envTruthy("MUTATE"), "-fats modes: set the fat mutate flag — touch AND change the fat's storage (default off: touch and read only)")
	flag.IntVar(&highloadTTL, "highload-ttl", envInt("HIGHLOAD_TTL", int(highload.DefaultTimeout)), "highload modes: query-id / validity window in seconds (default 3600; must match the deploy)")
	flag.IntVar(&highloadShards, "highload-shards", envInt("HIGHLOAD_SHARDS", highload.NumShards), "highload modes: 4 (default) or 16 existing relayers")

	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: spam [--mode MODE] <count> [freq] [flags]\n\n")
		fmt.Fprintf(os.Stderr, "Examples:\n")
		fmt.Fprintf(os.Stderr, "  spam 100000 400                          # jetton mode (default)\n")
		fmt.Fprintf(os.Stderr, "  spam --mode external 100000 400\n")
		fmt.Fprintf(os.Stderr, "  spam --mode gram --gram-value 0.001 100000 400\n")
		fmt.Fprintf(os.Stderr, "  spam --mode gassless-jetton --relay-value 0.01 100000 400\n")
		fmt.Fprintf(os.Stderr, "  spam --mode targets --targets big_accounts.txt 5000 400   # poke big-state accounts\n")
		fmt.Fprintf(os.Stderr, "  spam -c 100000 -f 400 -e http://devnet-01.toncenter.com --minter EQ...\n")
		fmt.Fprintf(os.Stderr, "  COUNT=100000 FREQ=400 spam               # legacy env\n\n")
		fmt.Fprintf(os.Stderr, "Flags:\n")
		flag.PrintDefaults()
	}
	flag.Parse()

	spamMode := normalizeMode(mode)
	if spamMode == "" {
		die("invalid --mode %q (expected 'jetton', 'jetton-fats', 'gram', 'gassless-gram', 'gassless-jetton', 'highload-jetton', 'highload-gram', 'external', 'targets', 'zerostate', or 'zerostate-fats')", mode)
	}
	// A mode decomposes into a sender kind (base) + whether the recipient is a fat load-target account.
	// base=="jetton" => the WalletSpam pool sender path (below); base=="zerostate" => the standalone W5 path.
	fatsTarget := strings.HasSuffix(spamMode, "-fats")
	base := strings.TrimSuffix(spamMode, "-fats")
	// The zerostate modes both send FROM the deterministically-generated wallet-v5 wallets and share the
	// same standalone run path, so several jetton-flavored checks below apply to both.
	w5Mode := base == "zerostate"

	// Whether the user explicitly set --minter / WALLET_MINTER (vs the SpamMinterTON default) — the zerostate
	// modes prefer the manifest minter and only override when the user really asked.
	minterExplicit := os.Getenv("WALLET_MINTER") != ""
	relayExplicit := false
	valueExplicit := false
	flag.Visit(func(f *flag.Flag) {
		if f.Name == "minter" {
			minterExplicit = true
		}
		if f.Name == "relay-value" {
			relayExplicit = true
		}
		if f.Name == "value" {
			valueExplicit = true
		}
	})

	// Positional args: 1 = count, 2 = freq. Only used if the corresponding
	// flag/env wasn't already set (i.e. value still equals zero default).
	args := flag.Args()
	if count == 0 && len(args) >= 1 {
		n, err := strconv.Atoi(args[0])
		if err != nil {
			die("invalid count %q: %v", args[0], err)
		}
		count = n
	}
	if freq == 0 && len(args) >= 2 {
		f, err := strconv.ParseFloat(args[1], 64)
		if err != nil {
			die("invalid freq %q: %v", args[1], err)
		}
		freq = f
	}
	// Go's flag parser stops at the first positional, so a flag placed AFTER count/freq is silently
	// dropped (e.g. `spam 100000 400 --mode external` would stay in jetton mode). count/freq are numeric,
	// so any leftover arg starting with '-' is always a misplaced flag — reject it loudly.
	for _, a := range args {
		if strings.HasPrefix(a, "-") {
			die("flag %q must come before the positional count/freq (Go stops flag parsing at the first positional arg)", a)
		}
	}

	if count <= 0 && !w5Mode {
		flag.Usage()
		die("\nCOUNT is required (positional arg, --count, or COUNT env)")
	}
	if (spamMode == "gram" || strings.HasPrefix(spamMode, "gassless-") || strings.HasPrefix(spamMode, "highload-")) && count < 2 {
		die("--mode %s requires at least 2 pool wallets", spamMode)
	}
	if freq > 0 && period > 0 {
		die("--freq and --period are mutually exclusive")
	}
	var rate float64
	switch {
	case freq > 0:
		rate = freq
	case period > 0:
		rate = 1000.0 / period
	default:
		rate = 10
	}
	if maxInflight == 0 {
		maxInflight = int(rate*2) + 10
	}
	sendTimeout := time.Duration(sendTimeoutMs) * time.Millisecond

	// Jetton value matters in the jetton AND zerostate senders (both -fats variants included) and the
	// gassless-jetton relayer path; parse it there so a bad value can't abort external/gram runs.
	var jettonValue tlb.Coins
	if base == "jetton" || w5Mode || spamMode == "gassless-jetton" || spamMode == "highload-jetton" {
		valueStr = jettonValueForMode(spamMode, valueStr, valueExplicit)
		v, err := tlb.FromTON(valueStr)
		if err != nil {
			die("invalid --value %q (GRAM, e.g. 0.05): %v", valueStr, err)
		}
		// Reject <= 0 at startup: a negative value panics later inside tlb.ToCell (in the per-send
		// goroutine, crashing the whole run), and 0 attaches no gas so every transfer fails on-chain.
		if v.Nano().Sign() <= 0 {
			die("--value must be positive GRAM (got %q): 0 or negative attaches no gas / panics on send", valueStr)
		}
		jettonValue = v
		if initMode != 0 && initMode != 1 {
			die("--init-mode must be 0 or 1 (got %d)", initMode)
		}
	}
	var gramValue tlb.Coins
	if spamMode == "gram" || spamMode == "gassless-gram" || spamMode == "highload-gram" {
		v, err := tlb.FromTON(gramValueStr)
		if err != nil || v.Nano().Sign() <= 0 {
			die("invalid --gram-value %q: expected positive GRAM", gramValueStr)
		}
		gramValue = v
	}
	var relayValue tlb.Coins
	if strings.HasPrefix(spamMode, "gassless-") || strings.HasPrefix(spamMode, "highload-") {
		v, err := resolveRelayValue(spamMode, gramValue, relayValueStr, relayExplicit)
		if err != nil {
			die("%v", err)
		}
		relayValue = v
	}

	// -fats modes: forward_ton must be > 0, else no transfer_notification fires and the fat is never touched.
	var forwardTONNano uint64
	if fatsTarget {
		fv, err := tlb.FromTON(forwardTONStr)
		if err != nil {
			die("invalid --forward-ton %q (GRAM, e.g. 0.01): %v", forwardTONStr, err)
		}
		if fv.Nano().Sign() <= 0 {
			die("--forward-ton must be > 0 in a -fats mode (got %q): the fat is only touched via the transfer notification", forwardTONStr)
		}
		forwardTONNano = fv.Nano().Uint64()
	}

	// Targets mode: load the big-state account list up front so a bad file aborts before we open backends.
	var targets []*address.Address
	if spamMode == "targets" {
		if targetsPath == "" {
			die("--mode targets requires --targets <file> (or TARGETS_FILE env)")
		}
		if targetNano <= 0 {
			die("--target-nano must be positive (got %d)", targetNano)
		}
		t, terr := loadTargets(targetsPath)
		if terr != nil {
			die("%v", terr)
		}
		targets = t
	}

	// jetton-fats: the WalletSpam pool sends to fat load-target accounts. Load the recipients (raw wc:hex,
	// same format as --mode targets) up front so a bad/missing file aborts before we open backends. Unlike
	// zerostate-fats this needs NO manifest (it reuses the pool's own key/minter), but --fats-file defaults
	// next to --manifest when one is given.
	var fatRecipients []*address.Address
	if spamMode == "jetton-fats" {
		ff := fatsFile
		if ff == "" && fatsManifest != "" {
			ff = filepath.Join(filepath.Dir(fatsManifest), "fats.addrs")
		}
		if ff == "" {
			die("--mode jetton-fats requires --fats-file <fats.addrs> (or --manifest to locate it)")
		}
		r, ferr := loadTargets(ff)
		if ferr != nil {
			die("%v", ferr)
		}
		fatRecipients = r
	}

	// check-balance threshold default is mode-aware: jetton needs the attached value + fee headroom; a
	// poke/external only needs to cover its own fwd fee + gas.
	if checkBalance && minSendNano <= 0 {
		if base == "jetton" {
			minSendNano = jettonValue.Nano().Int64() + 10_000_000
		} else {
			minSendNano = 10_000_000 // 0.01 GRAM
		}
	}
	gasless := strings.HasPrefix(spamMode, "gassless-") || strings.HasPrefix(spamMode, "highload-")
	if gasless && highloadShards != highload.NumShards && highloadShards != highload.MaxShards {
		die("--highload-shards must be 4 or 16")
	}
	if checkBalance && (spamMode == "gram" || gasless) {
		die("--check-balance is not supported in %s mode: pool balances can increase, so drained state is not monotonic", spamMode)
	}

	errFile, err := os.OpenFile(errorLogPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		die("open error log %s: %v", errorLogPath, err)
	}
	defer errFile.Close()
	errLog := log.New(errFile, "", log.LstdFlags|log.Lmicroseconds)
	errLog.Printf("=== spam start: mode=%s count=%d rate=%.1f ===", spamMode, count, rate)

	// spamLog is the detailed run log, tee'd to BOTH stdout and spam.log. Targets mode logs every poke
	// (sender wallet, target, size, result) here, so the exact target of each send is captured live — no
	// reconstructing from toncenter after the fact. Stats and the final summary also go here.
	spamFile, err := os.OpenFile(spamLogPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		die("open spam log %s: %v", spamLogPath, err)
	}
	defer spamFile.Close()
	spamLog := log.New(io.MultiWriter(os.Stdout, spamFile), "", log.LstdFlags)

	// Targets ordering + optional size annotation for the poke log.
	sequentialTargets := false
	targetSizeByAddr := map[string]int64{}
	if spamMode == "targets" || w5Mode || fatsTarget {
		switch strings.ToLower(strings.TrimSpace(orderStr)) {
		case "sequential", "seq", "order", "ordered", "inorder":
			sequentialTargets = true
		case "random", "rand", "shuffle", "":
			sequentialTargets = false
		default:
			die("--order must be 'random' or 'sequential' (got %q)", orderStr)
		}
	}
	if spamMode == "targets" {
		szPath := targetSizes
		if szPath == "" {
			szPath = targetsPath + ".sizes"
		}
		targetSizeByAddr = loadTargetSizes(szPath) // best-effort; missing file => sizes logged as 0
		spamLog.Printf("=== targets run: mode=targets count=%d rate=%.1f order=%s targets=%d file=%s sizes=%d ===",
			count, rate, map[bool]string{true: "sequential", false: "random"}[sequentialTargets], len(targets), targetsPath, len(targetSizeByAddr))
	}

	// Backend, run clock, and cancellation are shared by all modes.
	startedAt := time.Now()
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	var be backend.Backend
	if endpoint != "" {
		be = backend.NewHTTP(endpoint, apiKey)
		keySuffix := ""
		if apiKey != "" {
			keySuffix = " (with key)"
		}
		fmt.Printf("Backend: http %s%s\n", endpoint, keySuffix)
	} else {
		checkCtx, checkCancel := context.WithTimeout(ctx, 30*time.Second)
		_, checkErr := sidechain.RequireTVM(checkCtx, configPath, 0)
		checkCancel()
		if checkErr != nil {
			die("sidechain preflight: %v", checkErr)
		}
		lsBE, count_, err := backend.NewLSFromConfigFile(configPath)
		if err != nil {
			die("ls: %v", err)
		}
		be = lsBE
		fmt.Printf("Backend: lite %s (%d servers)\n", configPath, count_)
	}

	// The zerostate modes send FROM the deterministically-generated wallet-v5 wallets (not the WalletSpam
	// pool), so they are fully standalone and never build the WalletSpam pool below.
	if w5Mode {
		w5Minter := ""
		if minterExplicit {
			w5Minter = minterStr
		}
		// Recipient init mode: zerostate-fats forces 0 (the fat's jetton wallet is NOT pre-deployed, so a
		// full state_init must auto-deploy it); plain zerostate uses --init-mode (default 0), since its
		// recipients are already deployed in the zerostate — 1 (bare) is the cheapest correct option there
		// but 0 also works.
		w5InitMode := int32(0)
		if !fatsTarget {
			w5InitMode = int32(initMode)
		}
		runW5Spam(ctx, be, w5RunOpts{
			fatsTarget:   fatsTarget,
			fatsFile:     fatsFile,
			manifestPath: fatsManifest,
			seedHex:      fatsSeedHex,
			minterStr:    w5Minter,
			count:        count,
			value:        jettonValue,
			initMode:     w5InitMode,
			forwardTON:   forwardTONNano,
			mutate:       fatsMutate,
			sequential:   sequentialTargets,
			rate:         rate,
			maxInflight:  maxInflight,
			sendTimeout:  sendTimeout,
			noRetry:      noRetry,
		}, spamLog, errLog, startedAt)
		return
	}

	pub, priv, err := backend.LoadKeypairFromSeedFile(poolKeyPath)
	if err != nil {
		die("%v", err)
	}
	fmt.Printf("Pubkey: 0x%x\n", pub)

	if minterStr == "" {
		die("minter address is required (--minter or WALLET_MINTER): it is part of each WalletSpam's storage and fixes the pool address")
	}
	minter, err := address.ParseAddr(minterStr)
	if err != nil {
		die("invalid minter address %q: %v", minterStr, err)
	}

	code := walletspam.LoadCode(walletspam.CodeHex)
	jwCode := walletspam.LoadJettonWalletCode()

	probe := walletspam.New(0, pub, priv, minter, jwCode, code)
	fmt.Printf("Wallet[subwallet=0]: %s (minter=%s)\n", probe.Address().String(), minter.String())

	// highload relay modes: four highload-v3 relayers, one per top-two-bit shard bucket, derived from the
	// POOL keypair (same pool.key — no separate keyfile, to avoid confusion). The relayer for a send is
	// chosen by the TARGET's shard so the relay hop stays intra-shard. Deploy them first with
	// `cmd/highload deploy` (same pool key + ttl fix the addresses).
	var highloads []*highload.HighloadWallet
	if gasless {
		highloads = highload.NewShardWallets(pub, priv, uint32(highloadTTL), highloadShards)
		fmt.Printf("Highload relayers (ttl=%ds, pool key):\n", highloadTTL)
		for i := range highloads {
			fmt.Printf("  shard %x: sub=%d %s\n", i, highloads[i].SubwalletID(), highloads[i].Address())
		}
	}

	fmt.Printf("Count: %d, Target rate: %.1f req/s, MaxInflight: %d, SendTimeout: %s, Retry: %t\n",
		count, rate, maxInflight, sendTimeout, !noRetry)
	switch spamMode {
	case "jetton":
		fmt.Printf("Mode: jetton — op::transfer of 1 jetton to a random pool wallet, value=%s GRAM, init-mode=%d\n",
			jettonValue.String(), initMode)
	case "jetton-fats":
		order := map[bool]string{true: "sequential", false: "random"}[sequentialTargets]
		fmt.Printf("Mode: jetton-fats — pool wallet -> op::transfer of 1 jetton to a random fat (%d targets, %s), value=%s GRAM, forward=%.4f GRAM, mutate=%t, init-mode=%d\n",
			len(fatRecipients), order, jettonValue.String(), float64(forwardTONNano)/1e9, fatsMutate, initMode)
	case "gram":
		fmt.Printf("Mode: gram — ordinary transfers to random pool wallets, value=%s GRAM\n", gramValue.String())
	case "gassless-gram":
		fmt.Printf("Mode: gassless-gram — highload-batched GRAM transfers, value=%s GRAM, relay-value=%s GRAM\n",
			gramValue.String(), relayValue.String())
	case "gassless-jetton":
		fmt.Printf("Mode: gassless-jetton — highload-batched jetton transfers, jetton-value=%s GRAM, relay-value=%s GRAM, init-mode=%d\n",
			jettonValue.String(), relayValue.String(), initMode)
	case "highload-jetton":
		fmt.Printf("Mode: highload-jetton — %d highload-v3 shard relayers pay gas for pool wallets' jetton transfers, jetton-value=%s GRAM, relay-value=%s GRAM, init-mode=%d\n",
			highloadShards, jettonValue.String(), relayValue.String(), initMode)
	case "highload-gram":
		fmt.Printf("Mode: highload-gram — %d highload-v3 shard relayers pay gas for pool wallets' GRAM transfers, gram-value=%s GRAM, relay-value=%s GRAM\n",
			highloadShards, gramValue.String(), relayValue.String())
	case "targets":
		fmt.Printf("Mode: targets — pokes %d big-state accounts (%s) with %d nanoTON, bounceable, order=%s; per-send log -> stdout + %s\n",
			len(targets), targetsPath, targetNano, orderStr, spamLogPath)
	default:
		fmt.Println("Mode: external — signed externals with no message refs (seqno bump only)")
	}
	if checkBalance {
		fmt.Printf("check-balance: ON — skip wallets < %d nanoTON (%.4f GRAM), auto-stop at %d%% drained\n",
			minSendNano, float64(minSendNano)/1e9, drain.StopPercent)
	}
	fmt.Println("Starting spam loop...")

	p := pool.New(count, pub, priv, minter, jwCode, code)
	stats := &pool.Stats{}
	var batcher *highload.Batcher
	if gasless {
		batcher = highload.NewBatcher(ctx, highloads, be.SendExternalMessage, highload.BatchOptions{
			SendTimeout: func() time.Duration { return sendTimeout },
			NoRetry:     func() bool { return noRetry },
			OnAttempt: func(retry bool, n int) {
				if retry {
					stats.BatchRetry.Add(1)
					stats.Retried.Add(uint64(n))
				} else {
					stats.BatchSend.Add(1)
				}
			},
			OnAccepted: func(int) { stats.Batches.Add(1) },
		})
		defer batcher.Close()
		fmt.Println("Gasless batching: 500ms windows, always self-message, max 254 requests / 60 KiB; retries use the same address limiter")
	}

	// check_balance: gate each send on the sender wallet's on-chain balance. A wallet below minSendBig is
	// skipped (Stats.Skipped — never Sent/Failed, no seqno consumed) and marked drained so later picks skip
	// it with no RPC; once drain.StopPercent% of the pool is drained, onStop cancels the run. Off by default.
	var tracker *drain.Tracker
	var minSendBig *big.Int
	if checkBalance {
		minSendBig = big.NewInt(minSendNano)
		tracker = drain.New(count, func() {
			line := fmt.Sprintf("=== auto-stop: %d%%+ of pool drained (%d/%d) — top up the wallets ===",
				drain.StopPercent, tracker.Count(), count)
			fmt.Println(line)
			errLog.Println(line)
			cancel()
		})
	}

	// Gasless aliases share the mandatory highload batcher; only target seqnos are reserved.
	var sendFn func(ctx context.Context, e *pool.Entry, seqno uint32) error
	switch spamMode {
	case "external":
		sendFn = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			body := e.Wallet.BuildExternalBody(seqno, uint32(time.Now().Unix()+40))
			return be.SendExternalMessage(ctx, e.Wallet.Address(), body)
		}
	case "jetton":
		initMode32 := int32(initMode)
		sendFn = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			// Recipient owner = another random pool WalletSpam (its jetton wallet is pre-deployed). With
			// check-balance on, draw a live (non-drained) recipient so GRAM stays among funded wallets.
			recipientID := mathrand.Intn(count)
			if tracker != nil {
				recipientID = tracker.RandomLive()
			}
			dest := p.Get(recipientID).Wallet.Address()
			validUntil := uint32(time.Now().Unix() + 120)
			body, err := jetton.BuildTransferExternal(
				e.Wallet, seqno, jwCode, minter, jettonValue, initMode32,
				dest, mathrand.Intn(1_000_000), validUntil,
			)
			if err != nil {
				return err
			}
			return be.SendExternalMessage(ctx, e.Wallet.Address(), body)
		}
	case "jetton-fats":
		// Same WalletSpam pool sender as --mode jetton, but the recipient is a fat load-target account
		// (from fatRecipients) reached with forward_ton>0 so a transfer_notification lands ON the fat.
		// initMode 0 (default) auto-deploys the fat's jetton wallet under the pool minter — it is NOT
		// pre-deployed. Target picked at random (default) or sequentially (biggest-first) via a shared
		// atomic cursor, matching --mode targets / the W5 fats path.
		initMode32 := int32(initMode)
		mutate := fatsMutate
		var cursor uint64
		sendFn = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			idx := mathrand.Intn(len(fatRecipients))
			if sequentialTargets {
				idx = int((atomic.AddUint64(&cursor, 1) - 1) % uint64(len(fatRecipients)))
			}
			fat := fatRecipients[idx]
			validUntil := uint32(time.Now().Unix() + 120)
			body, err := jetton.BuildFatTransferExternal(
				e.Wallet, seqno, jwCode, minter, jettonValue, initMode32,
				fat, forwardTONNano, mutate, validUntil,
			)
			if err != nil {
				spamLog.Printf("fat w%d seqno=%d -> %s build FAIL: %v", e.Wallet.SubwalletID, seqno, fat.String(), err)
				return err
			}
			return be.SendExternalMessage(ctx, e.Wallet.Address(), body)
		}
	case "gram":
		// Each pool wallet sends an ordinary native GRAM transfer (with a random text comment) to a random
		// OTHER pool wallet — the "cheapest real user" load. Same signed-external path as jetton, just a
		// GRAM message instead of an op::transfer.
		sendFn = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			recipient := p.Get(randomOtherWallet(count, int(e.Wallet.SubwalletID)))
			body, err := gram.BuildTransferExternal(
				e.Wallet,
				seqno,
				uint32(time.Now().Add(120*time.Second).Unix()),
				recipient.Wallet.Address(),
				gramValue,
			)
			if err != nil {
				return err
			}
			return be.SendExternalMessage(ctx, e.Wallet.Address(), body)
		}
	case "highload-jetton", "gassless-jetton":
		// Gasless via a dedicated Highload-V3 relayer (one per shard, routed by the target's top two bits),
		// not another pool wallet. The highload signs the OUTER external (replay-protected by a query_id, not a
		// seqno, so no relayer contention) and funds an internal carrying the target's `sint` op::transfer of 1
		// jetton to a random other pool wallet. Single Send path: only the target's seqno is tracked (the sint
		// bumps it, exactly as the target's own external would).
		sendFn = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			recipient := p.Get(randomOtherWallet(count, int(e.Wallet.SubwalletID)))
			actionMessage, err := jetton.BuildTransferInternal(
				e.Wallet.Address(), jwCode, minter, jettonValue, int32(initMode),
				recipient.Wallet.Address(), e.Wallet.Address(), mathrand.Intn(1_000_000),
			)
			if err != nil {
				return err
			}
			return batcher.Submit(ctx, e.Wallet, seqno, relayValue, actionMessage)
		}
	case "highload-gram", "gassless-gram":
		// Like highload-jetton, but the target's signed action is a native GRAM transfer (with a random text
		// comment) to a random other pool wallet.
		sendFn = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			recipient := p.Get(randomOtherWallet(count, int(e.Wallet.SubwalletID)))
			actionMessage, err := gram.BuildTransferInternalWithComment(recipient.Wallet.Address(), gramValue, gram.RandomComment())
			if err != nil {
				return err
			}
			return batcher.Submit(ctx, e.Wallet, seqno, relayValue, actionMessage)
		}
	case "targets":
		// Each pool wallet pokes a big-state account: one bounceable internal carrying targetNano (1 by
		// default) with an empty body. The value nets ~0 (bounces back), but collating the poke forces the
		// recipient's cells to be loaded from CellDB — the whole point of the test. Target is picked at
		// random (default) or sequentially through the file (biggest-first, since it's size-sorted); a
		// shared atomic cursor makes sequential assignment monotonic across the concurrent senders.
		pokeVal := tlb.FromNanoTONU(uint64(targetNano))
		emptyBody := cell.BeginCell().EndCell()
		const sendModePoke = uint8(1 | 2) // PAY_FEES_SEPARATELY | IGNORE_ERRORS
		var cursor uint64
		sendFn = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			idx := mathrand.Intn(len(targets))
			if sequentialTargets {
				idx = int((atomic.AddUint64(&cursor, 1) - 1) % uint64(len(targets)))
			}
			target := targets[idx]
			validUntil := uint32(time.Now().Unix() + 120)
			internalCell, err := tlb.ToCell(&tlb.InternalMessage{
				IHRDisabled: true,
				Bounce:      true,
				DstAddr:     target,
				Amount:      pokeVal,
				Body:        emptyBody,
			})
			if err != nil {
				spamLog.Printf("poke #%d w%d seqno=%d -> %s build FAIL: %v", idx, e.Wallet.SubwalletID, seqno, target.String(), err)
				return err
			}
			body := e.Wallet.BuildExternalBodyWithMessage(seqno, validUntil, sendModePoke, internalCell)
			err = be.SendExternalMessage(ctx, e.Wallet.Address(), body)
			if err != nil {
				spamLog.Printf("poke #%d w%d seqno=%d -> %s (%dB) FAIL: %v", idx, e.Wallet.SubwalletID, seqno, target.String(), targetSizeByAddr[target.StringRaw()], err)
			} else {
				spamLog.Printf("poke #%d w%d seqno=%d -> %s (%dB) ok", idx, e.Wallet.SubwalletID, seqno, target.String(), targetSizeByAddr[target.StringRaw()])
			}
			return err
		}
	}

	cfg := pool.LoopConfig{
		Rate:        rate,
		MaxInflight: int64(maxInflight),
		SendTimeout: func() time.Duration { return sendTimeout },
		ErrLog:      errLog,
		StatsEvery:  1000,
		NoRetry:     func() bool { return noRetry },
		Send:        sendFn,
		FetchSeqno: func(ctx context.Context, e *pool.Entry) (uint32, error) {
			return be.GetSeqno(ctx, e.Wallet.Address())
		},
	}
	if gasless {
		cfg.BatchedSend = true
		cfg.RemotePendingFor = highload.BatchValidity + 10*time.Second
		cfg.SendTimeout = func() time.Duration { return highload.RequestTimeout(sendTimeout) }
	}
	if tracker != nil {
		cfg.Gate = func(ctx context.Context, e *pool.Entry) bool {
			id := int(e.Wallet.SubwalletID)
			if tracker.IsDrained(id) {
				return false // known-drained: skip with no RPC
			}
			bal, err := be.GetBalance(ctx, e.Wallet.Address())
			if err != nil {
				errLog.Printf("subwallet=%d addr=%s phase=checkBalance err=%+v", id, e.Wallet.Address().String(), err)
				return false // fail-closed: only send when balance is confirmed
			}
			if bal.Cmp(minSendBig) < 0 {
				tracker.Mark(id) // may trigger the auto-stop
				return false
			}
			return true
		}
	}

	lastReportTime := time.Now()
	var lastSent, lastFailed, lastThrottled uint64
	cfg.OnTick = func(s *pool.Stats) {
		spamLog.Println(pool.FormatStats(s, lastSent, lastFailed, lastThrottled, lastReportTime))
		lastReportTime = time.Now()
		lastSent = s.Sent.Load()
		lastFailed = s.Failed.Load()
		lastThrottled = s.Throttled.Load()
	}

	p.Run(ctx, cfg, stats)
	printSummary(startedAt, stats, spamLog, errLog)
}

func randomOtherWallet(count int, excluded int) int {
	id := mathrand.Intn(count - 1)
	if id >= excluded {
		id++
	}
	return id
}

// loadTargetSizes reads an account\tbytes file (bigstate's .sizes) into a map keyed by canonical raw
// address, for annotating the poke log. Best-effort: a missing/garbled file yields an empty map.
func loadTargetSizes(path string) map[string]int64 {
	m := map[string]int64{}
	data, err := os.ReadFile(path)
	if err != nil {
		return m
	}
	for _, line := range strings.Split(string(data), "\n") {
		f := strings.Fields(line)
		if len(f) != 2 {
			continue
		}
		a, err := address.ParseRawAddr(f[0])
		if err != nil {
			continue
		}
		var n int64
		fmt.Sscanf(f[1], "%d", &n)
		m[a.StringRaw()] = n
	}
	return m
}

func printSummary(startedAt time.Time, stats *pool.Stats, spamLog, errLog *log.Logger) {
	elapsed := time.Since(startedAt).Seconds()
	sent := stats.Sent.Load()
	failed := stats.Failed.Load()
	throttled := stats.Throttled.Load()
	skipped := stats.Skipped.Load()
	avg := 0.0
	if elapsed > 0 {
		avg = float64(sent) / elapsed
	}
	line := fmt.Sprintf("=== final: sent=%d failed=%d throttled=%d skipped=%d elapsed=%.1fs avg=%.1f sent/s ===",
		sent, failed, throttled, skipped, elapsed, avg)
	spamLog.Println(line) // stdout + spam.log
	if errLog != nil {
		errLog.Println(line)
	}
}
