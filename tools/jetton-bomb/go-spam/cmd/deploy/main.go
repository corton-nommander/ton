// Native Go deploy for the WalletSpam pool — no acton, no minter contract, no deployer wallet. Meant to
// be driven by the devnet `ton` script (Variant B in docs/DEVNET_INTEGRATION.md). Three phases:
//
//   deploy plan       — compute wallet0 + the exact funding amount; print eval-able KEY=VALUE lines.
//   deploy status     — exit 0 when the keyed pool is fully deployed, 10 when wallet0 is absent, and
//                       wait for completion when wallet0 exists but the terminal wallet is not ready.
//                       When absent, the `ton` script funds wallet0 (genesis main-wallet via wallet.fif).
//   deploy broadcast  — send ONE pool-key-signed external (with wallet0's state_init) that deploys
//                       wallet0 and fires the reproduce chain, then wait for the TERMINAL wallet (subwallet
//                       count — the causally-last one, deployed only after the whole chain unrolled) before
//                       printing DEPLOY DONE / exit 0. That means the ENTIRE pool is up (no lingering
//                       failed% from an unbuilt tail). Each poll it probes a spread of sample subwallets
//                       (get_wallet_data when DEPLOY_JETTON=1, else seqno) and draws a progress bar, so a
//                       multi-minute wait shows movement. If the tail never deploys, the chain ran out of
//                       fee cushion — raise --fee-slack (FEE_SLACK).
//
// Same pool.key + minter + params must be used for both phases and for the spammer (go-spam/build/spam).
// The kickoff external is byte-identical to the acton deploy (pkg/deploy/parity_test.go).
//
// Flags / env (nano = nanoGRAM, matching the acton deploy env):
//   -c, --count            COUNT           pool size, deploys wallets [0, count] (required, >=1)
//       --chunk            DEPLOY_CHUNK    children per driver hop, 1..253 (default 200)
//       --deploy-jetton    DEPLOY_JETTON   1/0 — also deploy each wallet's prepaid jetton wallet (default 1)
//       --wallet-value     WALLET_VALUE    nano GRAM each pool wallet keeps (default 100000000000 = 100; devnet GRAM is free)
//       --jetton-value     JETTON_VALUE    nano GRAM per jetton-wallet deploy (default 10000000 = 0.01)
//       --fee-slack        FEE_SLACK       nano GRAM per-wallet fee cushion, rolls down the chain (default 100000000 = 0.1)
//       --minter           WALLET_MINTER   jetton minter address (fixes pool addresses; default SpamMinterTON)
//   -m, --pool-key         POOL_KEY_FILE   pool key file, 32-byte ed25519 seed (auto: jetton-prepaid/pool.key)
//   -e, --endpoint         TONCENTER_ENDPOINT  Toncenter v2 root (status/broadcast phases); or:
//   -l, --ls-config        LS_CONFIG       TON global config path (status/broadcast, if no endpoint)
//   -k, --api-key          TONCENTER_API_KEY
//       --pool-timeout     POOL_READY_TIMEOUT   max wait for the terminal wallet to deploy (default 10m)
//       --poll-interval    POOL_POLL_INTERVAL   delay between readiness polls (default 3s)

package main

import (
	"context"
	crand "crypto/rand"
	"flag"
	"fmt"
	"io"
	"math/big"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
	"github.com/victor/jetton-bomb-go-spam/pkg/deploy"
	"github.com/victor/jetton-bomb-go-spam/pkg/sidechain"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

// SpamMinterTON is the default minter (matches cmd/bot and cmd/spam so all three target the same pool).
const SpamMinterTON = "kQCy3cWuUXredDmXUTipDo31Juda2Y10jfK9DYS0gBPAjD-X"

// poolMissingExitCode lets the orchestration distinguish an expected deploy from a broken or incomplete
// existing pool. Other failures keep the conventional exit code 1.
const poolMissingExitCode = 10

type poolState uint8

const (
	poolMissing poolState = iota
	poolReady
	poolDeploying
)

func envStr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func envInt(k string, def int) int {
	if v := os.Getenv(k); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
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

func envDuration(k string, def time.Duration) time.Duration {
	if v := os.Getenv(k); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			return d
		}
	}
	return def
}

func die(format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(1)
}

func main() {
	// Phase is the first positional arg.
	if len(os.Args) < 2 {
		die("usage: deploy <plan|status|broadcast> [flags]")
	}
	phase := os.Args[1]
	if phase != "plan" && phase != "status" && phase != "broadcast" && phase != "kickoff" {
		die("unknown phase %q (expected 'plan', 'status', 'broadcast', or 'kickoff')", phase)
	}
	fs := flag.NewFlagSet("deploy", flag.ExitOnError)

	var (
		count        int
		start        int
		chunk        int
		deployJetton bool
		walletValue  int64
		jettonValue  int64
		feeSlack     int64
		minterStr    string
		poolKeyPath  string
		endpoint     string
		apiKey       string
		configPath   string
		poolTimeout  time.Duration
		pollInterval time.Duration
	)
	fs.IntVar(&count, "count", envInt("COUNT", 0), "pool size (deploys wallets [start,count])")
	fs.IntVar(&count, "c", envInt("COUNT", 0), "short for --count")
	fs.IntVar(&start, "start", envInt("START_SUBWALLET", 0), "driver subwallet to kick off from (0=wallet0 full deploy; >0=mid-range 2nd chain covering (start,count])")
	fs.IntVar(&chunk, "chunk", envInt("DEPLOY_CHUNK", 200), "children per driver hop (1..253)")
	fs.BoolVar(&deployJetton, "deploy-jetton", envBool("DEPLOY_JETTON", true), "also deploy each wallet's jetton wallet")
	fs.Int64Var(&walletValue, "wallet-value", int64(envInt("WALLET_VALUE", 100000000000)), "nano GRAM kept per pool wallet")
	fs.Int64Var(&jettonValue, "jetton-value", int64(envInt("JETTON_VALUE", 10000000)), "nano GRAM per jetton-wallet deploy")
	fs.Int64Var(&feeSlack, "fee-slack", int64(envInt("FEE_SLACK", 100000000)), "nano GRAM per-wallet fee headroom (rolls down the chain as the fee cushion)")
	fs.StringVar(&minterStr, "minter", envStr("WALLET_MINTER", SpamMinterTON), "jetton minter address")
	fs.StringVar(&poolKeyPath, "pool-key", envStr("POOL_KEY_FILE", backend.DefaultPoolKeyFile()), "pool key file (32-byte seed)")
	fs.StringVar(&poolKeyPath, "m", envStr("POOL_KEY_FILE", backend.DefaultPoolKeyFile()), "short for --pool-key")
	fs.StringVar(&endpoint, "endpoint", envStr("TONCENTER_ENDPOINT", ""), "Toncenter v2 root (status/broadcast)")
	fs.StringVar(&endpoint, "e", envStr("TONCENTER_ENDPOINT", ""), "short for --endpoint")
	fs.StringVar(&apiKey, "api-key", envStr("TONCENTER_API_KEY", ""), "Toncenter API key")
	fs.StringVar(&apiKey, "k", envStr("TONCENTER_API_KEY", ""), "short for --api-key")
	fs.StringVar(&configPath, "ls-config", envStr("LS_CONFIG", "./config.json"), "TON global config path (status/broadcast, LS)")
	fs.StringVar(&configPath, "l", envStr("LS_CONFIG", "./config.json"), "short for --ls-config")
	fs.DurationVar(&poolTimeout, "pool-timeout", envDuration("POOL_READY_TIMEOUT", 10*time.Minute), "max wait for the terminal pool wallet to deploy (broadcast)")
	fs.DurationVar(&pollInterval, "poll-interval", envDuration("POOL_POLL_INTERVAL", 3*time.Second), "delay between readiness polls")
	_ = fs.Parse(os.Args[2:])

	if count < 1 {
		die("--count must be >= 1 (got %d)", count)
	}
	if start < 0 || start >= count {
		die("--start must be in [0, count) (got %d, count %d)", start, count)
	}
	if chunk < 1 || chunk > 253 {
		die("--chunk must be 1..253 (got %d)", chunk)
	}
	if pollInterval < 0 {
		die("--poll-interval must be >= 0 (got %s)", pollInterval)
	}
	if poolTimeout <= 0 {
		die("--pool-timeout must be > 0 (got %s)", poolTimeout)
	}

	// The `plan` phase generates pool.key on first run (like the acton deploy) so the flow is
	// self-contained without acton. `status` and `broadcast` require the SAME key `plan` produced (never generate,
	// or it would target a different, unfunded wallet0). For a fresh pool per run, delete pool.key before
	// `plan` (mirrors the legacy NEW=1 behavior); to keep a stable pool, leave it.
	if phase == "plan" {
		ensurePoolKey(poolKeyPath)
	}

	pub, priv, err := backend.LoadKeypairFromSeedFile(poolKeyPath)
	if err != nil {
		die("%v", err)
	}
	minter, err := address.ParseAddr(minterStr)
	if err != nil {
		die("invalid minter %q: %v", minterStr, err)
	}
	code := walletspam.LoadCode(walletspam.CodeHex)
	jwCode := walletspam.LoadJettonWalletCode()

	d := deploy.New(pub, priv, minter, jwCode, code, deploy.Params{
		Count:          count,
		StartSubwallet: uint32(start),
		Chunk:          uint16(chunk),
		DeployJetton:   deployJetton,
		WalletValue:    tlb.FromNanoTON(big.NewInt(walletValue)),
		JettonValue:    tlb.FromNanoTON(big.NewInt(jettonValue)),
		FeeSlack:       tlb.FromNanoTON(big.NewInt(feeSlack)),
	})
	wallet0 := d.Wallet0()

	switch phase {
	case "plan":
		// Eval-able output for the ton script: `eval "$(deploy plan ...)"`.
		fmt.Printf("DEPLOY_WALLET0=%s\n", wallet0.Address().String())
		fmt.Printf("DEPLOY_FUNDING_NANO=%s\n", d.Wallet0Funding().Nano().String())
		fmt.Printf("DEPLOY_MINTER=%s\n", minter.String())
		fmt.Printf("DEPLOY_POOL_PUBKEY=%x\n", pub)
		fmt.Printf("DEPLOY_COUNT=%d\n", count)

	case "status":
		be := makeBackend(endpoint, apiKey, configPath)
		ids, addrs, method := d.ReadinessSamples(progressSamples)
		state, seq := inspectPool(be, wallet0.Address(), addrs[len(addrs)-1], method)
		switch state {
		case poolReady:
			fmt.Printf("POOL READY (terminal subwallet %d is deployed; reusing %s)\n", count, poolKeyPath)
			return
		case poolMissing:
			fmt.Fprintf(os.Stderr, "POOL MISSING (wallet0 %s is not deployed; keeping key %s)\n",
				wallet0.Address().String(), poolKeyPath)
			os.Exit(poolMissingExitCode)
		case poolDeploying:
			fmt.Printf("POOL INCOMPLETE (wallet0 seqno=%d, terminal subwallet %d not ready); waiting via %s...\n",
				seq, count, method)
			if err := waitReady(os.Stdout, be, ids, addrs, method, pollInterval, poolTimeout); err != nil {
				die("existing pool is incomplete: %v\nThe kickoff cannot be replayed. Keep %s; check the original pool size or reset the chain.",
					err, poolKeyPath)
			}
			fmt.Printf("POOL READY (terminal subwallet %d finished deploying; reusing %s)\n", count, poolKeyPath)
			return
		}

	case "kickoff":
		// Send-only: fire the kickoff external and verify the driver came up (seqno -> 1), but do NOT wait
		// for the terminal. Used to start a mid-range 2nd chain (--start >0) alongside a running monitor
		// that already watches the terminal. Also works for --start 0 (a fire-and-forget wallet0 kickoff).
		body, err := d.KickoffBody()
		if err != nil {
			die("build kickoff: %v", err)
		}
		be := makeBackend(endpoint, apiKey, configPath)
		preCtx, preCancel := context.WithTimeout(context.Background(), 8*time.Second)
		if seq, err := be.GetSeqno(preCtx, wallet0.Address()); err == nil {
			preCancel()
			die("driver subwallet %d (%s) is already deployed (seqno=%d) — the kickoff is single-use (seqno 0).",
				start, wallet0.Address().String(), seq)
		}
		preCancel()
		fmt.Printf("Kickoff from subwallet %d (%s), covering (%d, %d], funding expected %s nano\n",
			start, wallet0.Address().String(), start, count, d.Wallet0Funding().Nano().String())
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		if err := be.SendExternalMessageWithInit(ctx, wallet0.Address(), body, wallet0.StateInit()); err != nil {
			cancel()
			die("send kickoff external: %v", err)
		}
		cancel()
		fmt.Println("Kickoff external sent; verifying driver deployed + accepted (seqno -> 1)...")
		if err := waitSeqno(be, wallet0.Address(), 1, 40, 3*time.Second); err != nil {
			die("verification failed: %v\n(likely driver underfunded, or endpoint/network issue)", err)
		}
		fmt.Printf("KICKOFF OK — subwallet %d chain is unrolling toward terminal %d (watch it via the running monitor).\n", start, count)

	case "broadcast":
		body, err := d.KickoffBody()
		if err != nil {
			die("build kickoff: %v", err)
		}
		be := makeBackend(endpoint, apiKey, configPath)
		// Pre-check: wallet0 must be UNINITIALIZED. The kickoff external is single-use (seqno 0), so it
		// is only ever accepted by a not-yet-deployed wallet0. If wallet0 already has code (GetSeqno
		// succeeds), a re-broadcast is rejected on-chain (InvalidSeqno) but /sendBoc still acks it — which
		// would make the seqno>=1 verification below false-report success. Refuse, and point at a fresh
		// pool. (GetSeqno errors on an uninitialized account — the expected, proceed case.)
		preCtx, preCancel := context.WithTimeout(context.Background(), 8*time.Second)
		if seq, err := be.GetSeqno(preCtx, wallet0.Address()); err == nil {
			preCancel()
			die("wallet0 %s is already deployed (seqno=%d) — the kickoff is single-use (seqno 0) and cannot re-run.\n"+
				"For a fresh pool: delete %s (or wipe the chain), then re-run plan + broadcast.",
				wallet0.Address().String(), seq, poolKeyPath)
		}
		preCancel()
		fmt.Printf("Broadcasting kickoff to wallet0 %s (funding expected %s nano)\n",
			wallet0.Address().String(), d.Wallet0Funding().Nano().String())
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		if err := be.SendExternalMessageWithInit(ctx, wallet0.Address(), body, wallet0.StateInit()); err != nil {
			cancel()
			die("send kickoff external: %v", err)
		}
		cancel()
		fmt.Println("Kickoff external sent; waiting for wallet0 to deploy + accept (seqno -> 1)...")
		if err := waitSeqno(be, wallet0.Address(), 1, 40, 3*time.Second); err != nil {
			die("verification failed: %v\n(likely wallet0 underfunded, or endpoint/network issue)", err)
		}
		// wallet0 accepted the kickoff, but the reproduce chain fans out over many blocks. Wait for the
		// TERMINAL wallet (subwallet count) — the causally-last one, deployed only after the whole chain
		// unrolled — so DEPLOY DONE means the ENTIRE pool is up (no lingering failed% from an unbuilt tail).
		// Sample a spread of subwallets each poll to draw a progress bar (a multi-minute wait would look
		// hung otherwise); the terminal (last sample) is the completion signal.
		ids, addrs, method := d.ReadinessSamples(progressSamples)
		fmt.Printf("Waiting for full pool deploy (terminal subwallet %d) via %s (poll %s, timeout %s)...\n",
			count, method, pollInterval, poolTimeout)
		if err := waitReady(os.Stdout, be, ids, addrs, method, pollInterval, poolTimeout); err != nil {
			die("pool readiness: %v\n(reproduce chain stalled — likely tail underfunded: raise --fee-slack, or check endpoint / raise --pool-timeout)", err)
		}
		fmt.Printf("WALLETSPAM MINTER=%s\n", minter.String())
		fmt.Printf("WALLETSPAM POOL_PUBKEY=%x\n", pub)
		fmt.Println("DEPLOY DONE (terminal wallet up — the whole pool is deployed)")
	}
}

// inspectPool distinguishes a reusable pool from an absent pool and a deployment already in progress.
// The terminal wallet is authoritative for readiness; wallet0 only tells us whether replaying the
// single-use kickoff would be unsafe.
func inspectPool(be backend.Backend, wallet0, terminal *address.Address, method string) (poolState, uint32) {
	deployed, _ := probeSamples(be, []*address.Address{terminal}, method)
	if deployed[0] {
		return poolReady, 0
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	seq, err := be.GetSeqno(ctx, wallet0)
	cancel()
	if err == nil {
		return poolDeploying, seq
	}
	return poolMissing, 0
}

// ensurePoolKey writes a fresh 32-byte ed25519 seed to path if it doesn't exist. The raw 32 bytes are
// exactly what backend.LoadKeypairFromSeedFile reads and what the acton deploy writes, so Go and acton
// derive the identical pool from the same file. The NOTE goes to stderr to keep `plan` stdout eval-able.
func ensurePoolKey(path string) {
	if _, err := os.Stat(path); err == nil {
		return
	}
	seed := make([]byte, 32)
	if _, err := crand.Read(seed); err != nil {
		die("generate pool key: %v", err)
	}
	if err := os.WriteFile(path, seed, 0600); err != nil {
		die("write pool key %s: %v", path, err)
	}
	fmt.Fprintf(os.Stderr, "NOTE generated a new pool key -> %s (32-byte ed25519 seed). BACK IT UP — it controls the whole pool.\n", path)
}

func makeBackend(endpoint, apiKey, configPath string) backend.Backend {
	if endpoint != "" {
		fmt.Printf("Backend: http %s\n", endpoint)
		return backend.NewHTTP(endpoint, apiKey)
	}
	checkCtx, checkCancel := context.WithTimeout(context.Background(), 30*time.Second)
	_, checkErr := sidechain.RequireTVM(checkCtx, configPath, 0)
	checkCancel()
	if checkErr != nil {
		die("sidechain preflight: %v", checkErr)
	}
	be, n, err := backend.NewLSFromConfigFile(configPath)
	if err != nil {
		die("ls: %v", err)
	}
	fmt.Printf("Backend: lite %s (%d servers)\n", configPath, n)
	return be
}

// progressSamples is how many subwallets across [0,count] broadcast probes each poll to estimate deploy
// progress. Small + fixed (UX only), so it stays far from a full [0,count] sweep.
const progressSamples = 20

// waitReady polls the sample addresses each round, rendering a progress bar to `out`, until the TERMINAL
// sample (last address, subwallet count) is deployed — or the timeout elapses (→ non-zero exit, so the
// devnet `ton` script aborts before spamming). The terminal is the causally-last wallet in the reproduce
// chain, so its deployment means the WHOLE pool is up (no lingering failed% from an unbuilt tail); the other
// samples just feed the bar so a multi-minute wait shows movement instead of looking hung.
func waitReady(out io.Writer, be backend.Backend, ids []uint32, addrs []*address.Address, method string, poll, timeout time.Duration) error {
	count := ids[len(ids)-1]
	termIdx := len(addrs) - 1
	total := len(addrs)
	start := time.Now()
	deadline := start.Add(timeout)
	for {
		deployed, termErr := probeSamples(be, addrs, method)
		n := 0
		for _, ok := range deployed {
			if ok {
				n++
			}
		}
		done := deployed[termIdx]
		fmt.Fprint(out, progressBar(n, total, count, time.Since(start), done))
		if done {
			return nil
		}
		if time.Now().After(deadline) {
			fmt.Fprintln(out)
			return fmt.Errorf("terminal wallet (subwallet %d) not deployed after %s (%d/%d samples up): %v", count, timeout, n, total, termErr)
		}
		time.Sleep(poll)
	}
}

// probeSamples probes every sample address once, concurrently (the set is small and fixed). deployed[i] is
// true when addrs[i] answers `method` without error and with a non-empty stack (an uninit account errors on
// LS / non-{0,1} exit_code on HTTP; the non-empty check mirrors GetSeqno). termErr is the last address's
// probe error (nil once deployed), surfaced on timeout to hint at endpoint issues. Each goroutine writes
// its own distinct slice index, so no lock is needed.
func probeSamples(be backend.Backend, addrs []*address.Address, method string) (deployed []bool, termErr error) {
	deployed = make([]bool, len(addrs))
	errs := make([]error, len(addrs))
	var wg sync.WaitGroup
	for i := range addrs {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
			stack, err := be.RunGetMethod(ctx, addrs[i], method)
			cancel()
			switch {
			case err == nil && len(stack) > 0:
				deployed[i] = true
			case err != nil:
				errs[i] = err
			default:
				errs[i] = fmt.Errorf("empty result stack")
			}
		}(i)
	}
	wg.Wait()
	return deployed, errs[len(errs)-1]
}

// progressBar renders a single \r-redrawn line, e.g.
// deploying pool [####------] 42%  ~4200/10000 wallets  8/20 samples  1m3s.
// `done` swaps the trailing clear for a final "terminal up" + newline.
func progressBar(deployed, total int, count uint32, elapsed time.Duration, done bool) string {
	const w = 20
	frac := 0.0
	if total > 0 {
		frac = float64(deployed) / float64(total)
	}
	filled := int(frac*float64(w) + 0.5)
	if filled > w {
		filled = w
	}
	bar := strings.Repeat("#", filled) + strings.Repeat("-", w-filled)
	approx := uint32(frac * float64(count))
	line := fmt.Sprintf("\rdeploying pool [%s] %3.0f%%  ~%d/%d wallets  %d/%d samples  %s",
		bar, frac*100, approx, count, deployed, total, elapsed.Round(time.Second))
	if done {
		return line + "  terminal up\n"
	}
	return line + "   " // pad to clear leftovers from a longer previous line
}

// waitSeqno polls addr's seqno until it equals want (or errors out). A WalletSpam that has accepted its
// kickoff reports seqno 1; before deploy the get-method errors, which we treat as "not ready yet".
func waitSeqno(be backend.Backend, addr *address.Address, want uint32, attempts int, delay time.Duration) error {
	var lastErr error
	for i := 0; i < attempts; i++ {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		got, err := be.GetSeqno(ctx, addr)
		cancel()
		if err == nil && got >= want {
			fmt.Printf("wallet0 seqno=%d (deployed + kickoff accepted)\n", got)
			return nil
		}
		if err != nil {
			lastErr = err
		} else {
			lastErr = fmt.Errorf("seqno=%d, want >=%d", got, want)
		}
		time.Sleep(delay)
	}
	return fmt.Errorf("wallet0 not ready after %d attempts: %w", attempts, lastErr)
}
