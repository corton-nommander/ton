// Telegram bot in Go — port of scripts/telegramTpsBot.ts.
//
// Env (mirrors TS bot):
//   TELEGRAM_BOT_TOKEN   required
//   WALLET_SECRET        V5R1 control wallet: hex ed25519 seed (32B) or private key (64B). Preferred.
//   WALLET_MNEMONIC      V5R1 control wallet: 24-word mnemonic. Used only if WALLET_SECRET is unset.
//   TONCENTER_ENDPOINT   optional (HTTP backend; default if set)
//   TONCENTER_API_KEY    optional
//   LS_CONFIG            optional, default ./config.json (LS backend; used if HTTP not set)
//   NETWORK_GLOBAL_ID    optional, default -239 (V5R1 wallet ID derivation)
//   POOL_KEY_FILE        optional, spam pool 32-byte ed25519 seed; auto-found at jetton-prepaid/pool.key
//                        whether run from the repo root or from go-spam/
//   WALLET_MINTER        optional, jetton minter address (part of every pool address; default SpamMinterTON).
//                        Set this to the MINTER your deploy printed, or the bot targets a different pool.
//   POOL_TOTAL           optional, the DEPLOYED pool size / number of subwallet ids (default 2500000).
//                        Alias: HAMSTER_POOL_SIZE. Set it to WALLETSPAM_COUNT you deployed, or the bot
//                        drives ids never stood up on-chain.
//   POOL_SIZE            optional, wallets driven per RUN (default 100000, = the legacy pool size). The
//                        pool is cut into POOL_TOTAL/POOL_SIZE fixed sub-pools and each run picks one at
//                        random, so the pool wears evenly. Live: /pool_size. Set it to POOL_TOTAL to drive
//                        the whole pool. Only /hamster_kombat ignores it and always drives everything.
//   JETTON_TRANSFER_VALUE optional, GRAM attached to each op::transfer (default 0.05). A float, not the
//                        cost — excesses return; net drain ~0.0022 GRAM/transfer. Contract floor ~0.0015.
//   GRAM_TRANSFER_VALUE  optional, native GRAM moved by /gram and /gassless_gram (default 0.001).
//   GASSLESS_RELAY_VALUE optional, GRAM the highload relayer attaches to each target `sint` request
//                        (jetton default 0.012; explicit overrides also apply to GRAM).
//                        Alias: GASLESS_RELAY_VALUE.
//   GASSLESS_GRAM_RELAY_VALUE optional, GRAM-specific relay override (takes precedence over the general
//                        override). Default GRAM_TRANSFER_VALUE + 0.001 = 0.002.
//                        Alias: GASLESS_GRAM_RELAY_VALUE.
//   GASSLESS_JETTON_VALUE optional, GRAM the target attaches to its op::transfer in /gassless_jetton
//                        (default 0.01). Separate from JETTON_TRANSFER_VALUE, which /jetton keeps.
//   HIGHLOAD_TTL         optional, highload query-id / validity window in seconds (default 3600). MUST
//                        match what `highload deploy` used, or the relayer addresses differ.
//   HIGHLOAD_SHARDS      optional, 4 (default/mainnet) or 16 (testnet experiment). Uses the same wallet
//                        addresses from the original 16-wallet set; only their routing changes.
//   HIGHLOAD_MIN_BALANCE optional, warn threshold per relayer in GRAM (default 1000). Below it the gasless
//                        status tick shows a warning and the audit chat gets a one-shot alert. No auto-topup.
//   FAT_SIZE_KB          optional, fat target tier for /fat_gram and /fat_jetton: 10|25|50|100|200 (default
//                        100). Live: /fat_size. Recipients are DERIVED (pkg/fatpool), nothing is stored.
//   FAT_JETTON_VALUE     optional, GRAM attached to each /fat_jetton op::transfer (default 0.05). Separate
//                        from JETTON_TRANSFER_VALUE: a fat transfer pays FAT_FORWARD_TON out of it AND gets
//                        no excesses back, so it must be comfortably above the forward + gas.
//   FAT_FORWARD_TON      optional, GRAM forwarded so a transfer_notification reaches the fat ITSELF
//                        (default 0.01). Must be > 0 or the fat's big state is never read.
//   FAT_MUTATE           optional, 1/true => set the fat's mutate flag: touch AND change its storage;
//                        default off = read-only touch (A/B). Toggle live with /fat_mutate on|off.
//   NO_RETRY             optional, 1/true => single-attempt sends (no refetch+resend on failure); default
//                        off (retries on). Toggle live with /retry on|off.
//   CHECK_BALANCE        optional, 1/true => gate jetton sends on wallet balance: skip drained wallets and
//                        auto-stop the /jetton run at 75% drained. Default off. Toggle live: /check_balance.
//   MIN_SEND_BALANCE     optional, /check_balance threshold in nanoTON (default = JETTON_TRANSFER_VALUE +
//                        0.01 GRAM headroom). A wallet below it can't fund a transfer, so it's skipped.
//   SHARDS_FILE          optional, default ./tps_shards.json
//   AUDIT_CHAT_ID        optional, private-channel chat id for the audit log (who started/stopped what,
//                        endpoint/config changes). Unset/0 disables it. The bot must be a member/admin of
//                        the channel. This channel never gets the periodic TPS status ticks.
//   BOT_LOG              optional, default bot.log
//   ERROR_LOG            optional, default errors.log
//   STREAMING_API_KEY    optional, Toncenter Streaming key (separate from send backend).
//   STREAMING_NETWORK    required with the key: mainnet or testnet.
//   STREAMING_ENABLED    optional, on by default when the key is set.
//   STREAMING_MAX_ADDRESSES optional, max bot addresses subscribed (default 90, max 90).
//
// CWD is the operative directory; config.json / mnemonic.txt / tps_shards.json must live there.

package main

import (
	"context"
	"crypto/ed25519"
	"fmt"
	"log"
	"math/big"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"

	tg "github.com/go-telegram/bot"
	"github.com/go-telegram/bot/models"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
	"github.com/victor/jetton-bomb-go-spam/pkg/fatpool"
	"github.com/victor/jetton-bomb-go-spam/pkg/gram"
	"github.com/victor/jetton-bomb-go-spam/pkg/highload"
	"github.com/victor/jetton-bomb-go-spam/pkg/streaming"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletv5"
)

func envStr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func envInt(k string, def int) int {
	v := os.Getenv(k)
	if v == "" {
		return def
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		return def
	}
	return n
}

func envInt32(k string, def int32) int32 {
	v := os.Getenv(k)
	if v == "" {
		return def
	}
	n, err := strconv.ParseInt(v, 10, 32)
	if err != nil {
		return def
	}
	return int32(n)
}

func envInt64(k string, def int64) int64 {
	v := os.Getenv(k)
	if v == "" {
		return def
	}
	n, err := strconv.ParseInt(v, 10, 64)
	if err != nil {
		return def
	}
	return n
}

func envBool(k string, def bool) bool {
	switch os.Getenv(k) {
	case "1", "true", "yes", "on":
		return true
	case "0", "false", "no", "off":
		return false
	}
	return def
}

func mustEnv(k string) string {
	v := os.Getenv(k)
	if v == "" {
		fmt.Fprintf(os.Stderr, "%s is not set\n", k)
		os.Exit(1)
	}
	return v
}

// loadControlKey derives the V5R1 control wallet keypair from WALLET_SECRET (hex ed25519 seed or 64-byte
// private key) if set, else from WALLET_MNEMONIC (24-word mnemonic). Both derive the SAME wallet, so you
// can move off the mnemonic (see cmd/derivekey) without changing the control-wallet address.
func loadControlKey() (ed25519.PublicKey, ed25519.PrivateKey, error) {
	if secret := os.Getenv("WALLET_SECRET"); secret != "" {
		return backend.LoadKeypairFromSecretHex(secret)
	}
	if m := os.Getenv("WALLET_MNEMONIC"); m != "" {
		return backend.LoadKeypairFromMnemonic(m)
	}
	return nil, nil, fmt.Errorf("set WALLET_SECRET (hex ed25519 seed/key) or WALLET_MNEMONIC")
}

func openLog(path string) *log.Logger {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open log %s: %v\n", path, err)
		os.Exit(1)
	}
	return log.New(f, "", log.LstdFlags|log.Lmicroseconds)
}

func main() {
	token := mustEnv("TELEGRAM_BOT_TOKEN")

	poolKeyFile := envStr("POOL_KEY_FILE", backend.DefaultPoolKeyFile())
	shardsFile := envStr("SHARDS_FILE", "./tps_shards.json")
	lsConfigPath := envStr("LS_CONFIG", "./config.json")
	httpEndpoint := envStr("TONCENTER_ENDPOINT", "")
	httpKey := envStr("TONCENTER_API_KEY", "")
	networkGlobalID := envInt32("NETWORK_GLOBAL_ID", walletv5.DefaultNetworkGlobalID)
	botLogPath := envStr("BOT_LOG", "./bot.log")
	errLogPath := envStr("ERROR_LOG", "./errors.log")

	botLogger := openLog(botLogPath)
	errLog := openLog(errLogPath)

	controlPub, controlPriv, err := loadControlKey()
	if err != nil {
		fmt.Fprintf(os.Stderr, "control wallet: %v\n", err)
		os.Exit(1)
	}
	controlWallet := walletv5.New(controlPub, controlPriv, networkGlobalID)

	spamPub, spamPriv, err := backend.LoadKeypairFromSeedFile(poolKeyFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "POOL_KEY_FILE %s: %v\n", poolKeyFile, err)
		fmt.Fprintf(os.Stderr, "  generate it once: cd jetton-prepaid && acton script scripts/deploy.tolk\n")
		os.Exit(1)
	}

	shardAddrs, err := loadShardAddresses(shardsFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "%v\n", err)
		os.Exit(1)
	}

	jettonMinter, err := address.ParseAddr(envStr("WALLET_MINTER", SpamMinterTON))
	if err != nil {
		fmt.Fprintf(os.Stderr, "parse SPAM_MINTER: %v\n", err)
		os.Exit(1)
	}

	spamCode := walletspam.LoadCode(walletspam.CodeHex)
	jwCode := walletspam.LoadJettonWalletCode()
	jettonValue, err := tlb.FromTON(envStr("JETTON_TRANSFER_VALUE", "0.05"))
	if err != nil || jettonValue.Nano().Sign() <= 0 {
		fmt.Fprintln(os.Stderr, "JETTON_TRANSFER_VALUE must be positive GRAM")
		os.Exit(1)
	}
	gramValue, err := tlb.FromTON(envStr("GRAM_TRANSFER_VALUE", "0.001"))
	if err != nil || gramValue.Nano().Sign() <= 0 {
		fmt.Fprintln(os.Stderr, "GRAM_TRANSFER_VALUE must be positive GRAM")
		os.Exit(1)
	}
	// Gasless funding pair: the target attaches 0.01 GRAM to its op::transfer and the highload relayer
	// sends it 0.012 (0.01 + ~0.002 of target-side fees) — the pair measured on testnet to complete the
	// whole jetton cascade even at init-mode 0.
	gasslessValue, err := tlb.FromTON(envStr("GASSLESS_RELAY_VALUE", envStr("GASLESS_RELAY_VALUE", "0.012")))
	if err != nil || gasslessValue.Nano().Sign() <= 0 {
		fmt.Fprintln(os.Stderr, "GASSLESS_RELAY_VALUE must be positive GRAM")
		os.Exit(1)
	}
	gasslessGramValue := gram.DefaultRelayValue(gramValue)
	if override := envStr("GASSLESS_GRAM_RELAY_VALUE", envStr("GASLESS_GRAM_RELAY_VALUE",
		envStr("GASSLESS_RELAY_VALUE", envStr("GASLESS_RELAY_VALUE", "")))); override != "" {
		gasslessGramValue, err = tlb.FromTON(override)
		if err != nil || gasslessGramValue.Nano().Sign() <= 0 {
			fmt.Fprintln(os.Stderr, "GASSLESS_GRAM_RELAY_VALUE must be positive GRAM")
			os.Exit(1)
		}
	}
	gasslessJettonValue, err := tlb.FromTON(envStr("GASSLESS_JETTON_VALUE", "0.01"))
	if err != nil || gasslessJettonValue.Nano().Sign() <= 0 {
		fmt.Fprintln(os.Stderr, "GASSLESS_JETTON_VALUE must be positive GRAM")
		os.Exit(1)
	}
	// Relayer low-balance warning threshold (no auto-topup — the bot only warns).
	highloadMinBalance, err := tlb.FromTON(envStr("HIGHLOAD_MIN_BALANCE", "1000"))
	if err != nil || highloadMinBalance.Nano().Sign() <= 0 {
		fmt.Fprintln(os.Stderr, "HIGHLOAD_MIN_BALANCE must be positive GRAM")
		os.Exit(1)
	}
	fatForward, err := tlb.FromTON(envStr("FAT_FORWARD_TON", "0.01"))
	if err != nil || fatForward.Nano().Sign() <= 0 {
		fmt.Fprintln(os.Stderr, "FAT_FORWARD_TON must be positive GRAM (0 never reaches the fat itself)")
		os.Exit(1)
	}
	// A fat transfer is NOT funded like a pool-to-pool one: the forward is paid out of the attached value
	// and nothing comes back (response_destination is addr_none), so the attached value must cover the
	// forward plus every hop's gas. Own knob, defaulted high, and asserted to leave room over the forward.
	fatJettonValue, err := tlb.FromTON(envStr("FAT_JETTON_VALUE", "0.05"))
	if err != nil || fatJettonValue.Nano().Sign() <= 0 {
		fmt.Fprintln(os.Stderr, "FAT_JETTON_VALUE must be positive GRAM")
		os.Exit(1)
	}
	if fatJettonValue.Nano().Cmp(new(big.Int).Add(fatForward.Nano(), big.NewInt(20_000_000))) < 0 {
		fmt.Fprintf(os.Stderr,
			"FAT_JETTON_VALUE (%s) must exceed FAT_FORWARD_TON (%s) by at least 0.02 GRAM of gas headroom\n",
			fatJettonValue.String(), fatForward.String())
		os.Exit(1)
	}

	r := &Runtime{
		wallet:        controlWallet,
		shardAddrs:    shardAddrs,
		spamPub:       spamPub,
		spamPriv:      spamPriv,
		spamCode:      spamCode,
		poolTotal:     envInt("POOL_TOTAL", envInt("HAMSTER_POOL_SIZE", DefaultPoolTotal)),
		jettonValue:   jettonValue,
		gramValue:     gramValue,
		gasslessValue: gasslessValue,
		jettonMinter:  jettonMinter,
		jwCode:        jwCode,
		defaultLSPath: lsConfigPath,
		auditChatID:   envInt64("AUDIT_CHAT_ID", 0),
		logger:        botLogger,
		errLog:        errLog,
		botToken:      token,

		gasslessJettonValue: gasslessJettonValue,
		gasslessGramValue:   gasslessGramValue,
		highloadTTL:         uint32(envInt("HIGHLOAD_TTL", int(highload.DefaultTimeout))),
		highloadCount:       envInt("HIGHLOAD_SHARDS", highload.NumShards),
		fatForwardTON:       fatForward.Nano().Uint64(),
		fatJettonValue:      fatJettonValue,
	}
	r.fatMutate.Store(envBool("FAT_MUTATE", false))
	// Wallets per run. Default 100k = the size the legacy pool had, so a default run drives the same
	// number of contracts as before the sub-pool scheme.
	// POOL_TOTAL is the DEPLOYED pool size. Defaulting it is only safe on the big testnet pool: on any
	// other deployment an unset POOL_TOTAL makes runs address subwallet ids that were never stood up.
	// Warn loudly rather than let that pass silently (it is invisible until sends start failing).
	if os.Getenv("POOL_TOTAL") == "" && os.Getenv("HAMSTER_POOL_SIZE") == "" {
		msg := fmt.Sprintf("POOL_TOTAL is not set — assuming the deployed pool is %d wallets. "+
			"If you deployed fewer, set POOL_TOTAL to that count or runs will target undeployed wallets.",
			DefaultPoolTotal)
		fmt.Fprintf(os.Stderr, "WARNING: %s\n", msg)
		botLogger.Printf("[WARN] %s", msg)
	}
	if r.poolTotal < 1 {
		fmt.Fprintf(os.Stderr, "POOL_TOTAL=%d must be at least 1\n", r.poolTotal)
		os.Exit(1)
	}
	subPool := envInt("POOL_SIZE", DefaultSubPoolSize)
	if subPool < 1 {
		fmt.Fprintf(os.Stderr, "POOL_SIZE=%d must be at least 1\n", subPool)
		os.Exit(1)
	}
	// Clamped, not rejected: a small deployment (e.g. a 5k mainnet pool) with the default 100k POOL_SIZE
	// must simply drive the whole pool, not refuse to start.
	r.subPoolSize.Store(int64(subPool))
	if subPool > r.poolTotal {
		botLogger.Printf("[INFO] POOL_SIZE=%d exceeds POOL_TOTAL=%d — driving the whole pool per run",
			subPool, r.poolTotal)
	}
	r.highloadMinBalance.Store(highloadMinBalance.Nano().Int64())
	if r.highloadCount != highload.NumShards && r.highloadCount != highload.MaxShards {
		fmt.Fprintf(os.Stderr, "HIGHLOAD_SHARDS=%d must be 4 or 16\n", r.highloadCount)
		os.Exit(1)
	}
	fatSize := envInt("FAT_SIZE_KB", DefaultFatSizeKB)
	if !fatpool.HasSize(fatSize) {
		fmt.Fprintf(os.Stderr, "FAT_SIZE_KB=%d is not a deployed tier (have %v)\n", fatSize, fatpool.Sizes())
		os.Exit(1)
	}
	r.fatSize.Store(int32(fatSize))
	r.timeoutMs.Store(1500)
	r.errorsEnabled.Store(true)
	r.jettonInitMode.Store(envInt32("JETTON_INIT_MODE", 0)) // 0 = classic full state_init (default)
	r.noRetry.Store(envBool("NO_RETRY", false))             // false = refetch+resend on failure (default)
	// /check_balance gate: default threshold = attached transfer value + 0.01 GRAM (gas/fwd headroom);
	// below it a wallet can't fund a send. MIN_SEND_BALANCE (nanoTON) overrides. Gate off unless CHECK_BALANCE.
	r.minSendBalance.Store(envInt64("MIN_SEND_BALANCE", r.jettonValue.Nano().Int64()+10_000_000))
	r.checkBalance.Store(envBool("CHECK_BALANCE", false))
	if key := os.Getenv("STREAMING_API_KEY"); key != "" {
		network := os.Getenv("STREAMING_NETWORK")
		if network != "mainnet" && network != "testnet" {
			fmt.Fprintln(os.Stderr, "STREAMING_NETWORK must be mainnet or testnet when STREAMING_API_KEY is set")
			os.Exit(1)
		}
		host := "toncenter.com"
		if network == "testnet" {
			host = "testnet.toncenter.com"
		}
		r.streamMemory = streaming.NewMemory()
		r.stream = streaming.New("wss://"+host+"/api/streaming/v2/ws", key, envInt("STREAMING_MAX_ADDRESSES", 90), r.streamMemory, nil)
		if envBool("STREAMING_ENABLED", true) {
			r.stream.On()
		}
		defer r.stream.Close()
	}

	if httpEndpoint != "" {
		r.defaultHTTP = backend.NewHTTP(httpEndpoint, httpKey)
		r.SetBackend(r.defaultHTTP)
		botLogger.Printf("[INFO] backend default http endpoint=%s hasKey=%t", httpEndpoint, httpKey != "")
	} else {
		if _, err := os.Stat(lsConfigPath); err != nil {
			fmt.Fprintf(os.Stderr, "no TONCENTER_ENDPOINT and LS_CONFIG=%s missing: %v\n", lsConfigPath, err)
			os.Exit(1)
		}
		be, count, err := backend.NewLSFromConfigFile(lsConfigPath)
		if err != nil {
			fmt.Fprintf(os.Stderr, "LS init %s: %v\n", lsConfigPath, err)
			os.Exit(1)
		}
		r.SetBackend(be)
		botLogger.Printf("[INFO] backend default lite (%d servers) config=%s", count, lsConfigPath)
	}

	probe := walletspam.New(0, spamPub, spamPriv, jettonMinter, jwCode, spamCode)
	fmt.Printf("Bot V5R1 wallet: %s\n", controlWallet.Address().String())
	fmt.Printf("Spam pool wallet[0]: %s\n", probe.Address().String())
	fmt.Printf(
		"Spam pool: %s wallets, %s per run (%d sub-pools)  minter: %s  jettonValue: %s GRAM  gramValue: %s GRAM\n",
		groupDigits(r.poolTotal),
		groupDigits(r.SubPoolSize()),
		r.poolTotal/r.SubPoolSize(),
		jettonMinter.String(),
		r.jettonValue.String(),
		r.gramValue.String(),
	)
	fmt.Printf("Fat modes: jettonValue %s GRAM  forward %s GRAM  mutate %t\n",
		r.fatJettonValue.String(), fatForward.String(), r.fatMutate.Load())
	// The gasless modes run off the four highload relayers: print them so the operator can fund/verify them
	// before starting a run (the bot never funds them; `highload deploy` does).
	relayers := r.initHighloads()
	fmt.Printf("Gasless relay: jettonValue %s GRAM  jettonRelayValue %s GRAM  gramRelayValue %s GRAM  warn below %s TON\n",
		r.gasslessJettonValue.String(), r.relayValue().String(),
		r.gramRelayValue().String(),
		tlb.FromNanoTON(big.NewInt(r.highloadMinBalance.Load())).String())
	fmt.Printf("Highload relayers (ttl %ds):\n", r.highloadTTL)
	for i := range relayers {
		fmt.Printf("  shard %x: %s\n", i, relayers[i].Address())
	}
	botLogger.Printf("[INFO] bot starting wallet=%s spamPool[0]=%s poolTotal=%d subPool=%d minter=%s",
		controlWallet.Address().String(), probe.Address().String(), r.poolTotal, r.SubPoolSize(), jettonMinter.String())

	defaultHandler := func(ctx context.Context, b *tg.Bot, update *models.Update) {
		// fallback for documents that arrived without a /command
		if update.Message != nil && update.Message.Document != nil {
			r.handleDocument(ctx, b, update)
		}
	}

	b, err := tg.New(token, tg.WithDefaultHandler(defaultHandler))
	if err != nil {
		fmt.Fprintf(os.Stderr, "tg.New: %v\n", err)
		os.Exit(1)
	}
	r.bot = b // enables audit() (no-op unless AUDIT_CHAT_ID is set)
	if r.auditChatID != 0 {
		botLogger.Printf("[INFO] audit log enabled -> chat %d", r.auditChatID)
	}

	b.RegisterHandler(tg.HandlerTypeMessageText, "start", tg.MatchTypeCommandStartOnly, r.handleStart)
	b.RegisterHandler(tg.HandlerTypeMessageText, "stop", tg.MatchTypeCommandStartOnly, r.handleStopAll)
	b.RegisterHandler(tg.HandlerTypeMessageText, "static_stop", tg.MatchTypeCommandStartOnly, r.handleStaticStop)
	b.RegisterHandler(tg.HandlerTypeMessageText, "shards", tg.MatchTypeCommandStartOnly, r.handleShards)
	b.RegisterHandler(tg.HandlerTypeMessageText, "shards_stop", tg.MatchTypeCommandStartOnly, r.handleShardsStop)
	b.RegisterHandler(tg.HandlerTypeMessageText, "jetton", tg.MatchTypeCommandStartOnly, r.handleJetton)
	b.RegisterHandler(tg.HandlerTypeMessageText, "jetton_stop", tg.MatchTypeCommandStartOnly, r.handleJettonStop)
	b.RegisterHandler(tg.HandlerTypeMessageText, "hamster_kombat", tg.MatchTypeCommandStartOnly, r.handleHamster)
	b.RegisterHandler(tg.HandlerTypeMessageText, "hamster_kombat_stop", tg.MatchTypeCommandStartOnly, r.handleHamsterStop)
	b.RegisterHandler(tg.HandlerTypeMessageText, "gram", tg.MatchTypeCommandStartOnly, r.handleGram)
	b.RegisterHandler(tg.HandlerTypeMessageText, "gram_stop", tg.MatchTypeCommandStartOnly, r.handleGramStop)
	b.RegisterHandler(tg.HandlerTypeMessageText, "gassless_gram", tg.MatchTypeCommandStartOnly, r.handleGasslessGram)
	b.RegisterHandler(tg.HandlerTypeMessageText, "gassless_gram_stop", tg.MatchTypeCommandStartOnly, r.handleGasslessGramStop)
	b.RegisterHandler(tg.HandlerTypeMessageText, "gassless_jetton", tg.MatchTypeCommandStartOnly, r.handleGasslessJetton)
	b.RegisterHandler(tg.HandlerTypeMessageText, "gassless_jetton_stop", tg.MatchTypeCommandStartOnly, r.handleGasslessJettonStop)
	b.RegisterHandler(tg.HandlerTypeMessageText, "fat_gram", tg.MatchTypeCommandStartOnly, r.handleFatGram)
	b.RegisterHandler(tg.HandlerTypeMessageText, "fat_gram_stop", tg.MatchTypeCommandStartOnly, r.handleFatGramStop)
	b.RegisterHandler(tg.HandlerTypeMessageText, "fat_jetton", tg.MatchTypeCommandStartOnly, r.handleFatJetton)
	b.RegisterHandler(tg.HandlerTypeMessageText, "fat_jetton_stop", tg.MatchTypeCommandStartOnly, r.handleFatJettonStop)
	b.RegisterHandler(tg.HandlerTypeMessageText, "fat_size", tg.MatchTypeCommandStartOnly, r.handleFatSize)
	b.RegisterHandler(tg.HandlerTypeMessageText, "fat_count", tg.MatchTypeCommandStartOnly, r.handleFatCount)
	b.RegisterHandler(tg.HandlerTypeMessageText, "fat_mutate", tg.MatchTypeCommandStartOnly, r.handleFatMutate)
	b.RegisterHandler(tg.HandlerTypeMessageText, "pool_size", tg.MatchTypeCommandStartOnly, r.handlePoolSize)
	b.RegisterHandler(tg.HandlerTypeMessageText, "help", tg.MatchTypeCommandStartOnly, r.handleHelp)
	b.RegisterHandler(tg.HandlerTypeMessageText, "status", tg.MatchTypeCommandStartOnly, r.handleStatus)
	b.RegisterHandler(tg.HandlerTypeMessageText, "external", tg.MatchTypeCommandStartOnly, r.handleExternal)
	b.RegisterHandler(tg.HandlerTypeMessageText, "external_stop", tg.MatchTypeCommandStartOnly, r.handleExternalStop)
	b.RegisterHandler(tg.HandlerTypeMessageText, "api", tg.MatchTypeCommandStartOnly, r.handleAPI)
	b.RegisterHandler(tg.HandlerTypeMessageText, "config", tg.MatchTypeCommandStartOnly, r.handleConfig)
	b.RegisterHandler(tg.HandlerTypeMessageText, "timeout", tg.MatchTypeCommandStartOnly, r.handleTimeout)
	b.RegisterHandler(tg.HandlerTypeMessageText, "errors", tg.MatchTypeCommandStartOnly, r.handleErrors)
	b.RegisterHandler(tg.HandlerTypeMessageText, "initmode", tg.MatchTypeCommandStartOnly, r.handleInitMode)
	b.RegisterHandler(tg.HandlerTypeMessageText, "retry", tg.MatchTypeCommandStartOnly, r.handleRetry)
	b.RegisterHandler(tg.HandlerTypeMessageText, "check_balance", tg.MatchTypeCommandStartOnly, r.handleCheckBalance)
	b.RegisterHandler(tg.HandlerTypeMessageText, "streaming", tg.MatchTypeCommandStartOnly, r.handleStreaming)

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	go r.reportStatus(ctx, os.Getenv("BOT_STATUS_URL"), os.Getenv("BOT_STATUS_SECRET"), os.Getenv("BOT_STATUS_NETWORK"))

	if _, err := b.SetMyCommands(ctx, &tg.SetMyCommandsParams{Commands: []models.BotCommand{
		{Command: "help", Description: "Command cheatsheet + current status"},
		{Command: "status", Description: "Current status only (backend, loops, relayers)"},
		{Command: "start", Description: "Start TPS on 1 shard — /start <tps>"},
		{Command: "stop", Description: "Stop EVERYTHING that is running"},
		{Command: "shards", Description: "Start TPS on all 16 shards — /shards <tps per contract>"},
		{Command: "jetton", Description: "Start jetton spam (100k pool) — /jetton <freq>"},
		{Command: "hamster_kombat", Description: "Start jetton spam over the full 2.5M pool — /hamster_kombat <freq>"},
		{Command: "gram", Description: "Start ordinary GRAM transfers — /gram <freq>"},
		{Command: "gassless_gram", Description: "Start relayed GRAM transfers — /gassless_gram <freq>"},
		{Command: "gassless_jetton", Description: "Start highload-relayed jetton transfers — /gassless_jetton <freq>"},
		{Command: "fat_gram", Description: "GRAM transfers to big-state fat accounts — /fat_gram <freq>"},
		{Command: "fat_jetton", Description: "Jetton transfers to big-state fat accounts — /fat_jetton <freq>"},
		{Command: "fat_size", Description: "Fat target size in KB — /fat_size 10|25|50|100|200"},
		{Command: "fat_count", Description: "Fat accounts aimed at per run — /fat_count <n> (default all)"},
		{Command: "pool_size", Description: "Wallets per run — /pool_size <n> (default 100000)"},
		{Command: "fat_mutate", Description: "Mutate the fat's storage or only read it — /fat_mutate on|off"},
		{Command: "external", Description: "Start external spam — /external <freq>"},
		{Command: "config", Description: "Upload liteserver config .json to use liteservers"},
		{Command: "api", Description: "Switch to HTTP backend — /api [url] [key]"},
		{Command: "timeout", Description: "Set per-send timeout in ms — /timeout 1500"},
		{Command: "initmode", Description: "Jetton init mode — /initmode 0 (full state_init) | 1 (bare)"},
		{Command: "retry", Description: "Refetch+resend on failed send — /retry on|off"},
		{Command: "check_balance", Description: "Skip drained wallets + stop at 75% — /check_balance on|off"},
		{Command: "streaming", Description: "Control delivery sampling — /streaming on|off"},
	}}); err != nil {
		botLogger.Printf("[WARN] SetMyCommands: %v", err)
	}

	botLogger.Printf("[INFO] bot polling started")
	b.Start(ctx)
	botLogger.Printf("[INFO] bot stopped")

	_ = time.Second
}
