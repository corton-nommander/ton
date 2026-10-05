// Command highload deploys and inspects the four gasless Highload-V3 relayers used by
// `spam --mode highload-jetton|highload-gram`. One relayer lands in each top-two-bit basechain shard bucket;
// a WalletSpam target's gasless request is relayed by the highload in its own shard, keeping the relay hop
// intra-shard. All four retain addresses from the old 16-wallet set (nibbles 0/4/8/c) and use the POOL key.
// The deploy funds + deploys all four in ONE signed external from the V5R1 control wallet.
//
// Subcommands:
//
//	grind    print the four shard relayer addresses (no network)
//	deploy   fund + deploy all four from the control wallet (skips already-deployed ones)
//	status   report each relayer's deployed state + GRAM balance
//
// The code cell is the canonical highload-v3r1 BoC embedded in pkg/highload (pulled from mainnet, not
// recompiled). --pool-key and --ttl must match what `spam` uses, or the addresses differ.
//
// Backend: --ls-config/-l (LS) or -e/--api-key (toncenter HTTP v2). Env mirrors the flags.
package main

import (
	"context"
	"crypto/ed25519"
	"fmt"
	"math/big"
	"os"
	"strconv"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
	"github.com/victor/jetton-bomb-go-spam/pkg/highload"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletv5"
)

func main() {
	if len(os.Args) < 2 {
		usage()
	}
	sub := os.Args[1]
	args := os.Args[2:]

	switch sub {
	case "grind":
		cmdGrind(args)
	case "deploy":
		cmdDeploy(args)
	case "status":
		cmdStatus(args)
	case "-h", "--help", "help":
		usage()
	default:
		fmt.Fprintf(os.Stderr, "unknown subcommand %q\n\n", sub)
		usage()
	}
}

func usage() {
	fmt.Fprintf(os.Stderr, `Usage: highload <grind|deploy|status> [flags]

  grind    print the four shard relayer addresses (offline)
  deploy   fund + deploy all four relayers from the V5R1 control wallet
  status   report each relayer's deployed state + balance

Common flags:
  --pool-key PATH       pool key file (raw 32-byte seed) — relayers share it; must already exist
  --ttl N               query-id / validity window seconds (default %d; MUST match spam)
  --shards N            4 (default) or 16 relayers
  --fund TON            (deploy) GRAM funded per relayer (default 500)
  -l, --ls-config PATH  LS global config (else HTTP)
  -e, --endpoint URL    toncenter v2 endpoint
  -k, --api-key KEY     toncenter API key
  --network-id N        V5R1 network global id (default %d)

Env: POOL_KEY_FILE, HIGHLOAD_TTL, HIGHLOAD_SHARDS, LS_CONFIG, TONCENTER_ENDPOINT, TONCENTER_API_KEY,
     WALLET_SECRET / WALLET_MNEMONIC (control wallet, deploy only).
`, highload.DefaultTimeout, walletv5.DefaultNetworkGlobalID)
	os.Exit(2)
}

// commonFlags is a tiny hand-rolled parser so every subcommand shares the same flag names without a
// FlagSet-per-command dance. Unknown flags are reported.
type commonFlags struct {
	keyPath   string
	ttl       int
	shards    int
	fundTON   string
	lsConfig  string
	endpoint  string
	apiKey    string
	networkID int
	dry       bool
}

func parseFlags(args []string) commonFlags {
	cf := commonFlags{
		keyPath:   envStr("POOL_KEY_FILE", backend.DefaultPoolKeyFile()),
		ttl:       envIntv("HIGHLOAD_TTL", int(highload.DefaultTimeout)),
		shards:    envIntv("HIGHLOAD_SHARDS", highload.NumShards),
		fundTON:   "500",
		lsConfig:  envStr("LS_CONFIG", ""),
		endpoint:  envStr("TONCENTER_ENDPOINT", ""),
		apiKey:    envStr("TONCENTER_API_KEY", ""),
		networkID: int(walletv5.DefaultNetworkGlobalID),
	}
	for i := 0; i < len(args); i++ {
		a := args[i]
		next := func() string {
			i++
			if i >= len(args) {
				die("flag %s needs a value", a)
			}
			return args[i]
		}
		switch a {
		case "--pool-key", "--highload-key", "--key", "-m":
			cf.keyPath = next()
		case "--ttl":
			cf.ttl = atoi(next())
		case "--shards":
			cf.shards = atoi(next())
		case "--fund":
			cf.fundTON = next()
		case "-l", "--ls-config":
			cf.lsConfig = next()
		case "-e", "--endpoint":
			cf.endpoint = next()
		case "-k", "--api-key":
			cf.apiKey = next()
		case "--network-id":
			cf.networkID = atoi(next())
		case "--dry", "--dry-run":
			cf.dry = true
		default:
			die("unknown flag %q", a)
		}
	}
	if cf.shards != highload.NumShards && cf.shards != highload.MaxShards {
		die("--shards must be 4 or 16")
	}
	return cf
}

func cmdGrind(args []string) {
	cf := parseFlags(args)
	pub, _ := mustKey(cf.keyPath)
	set := highload.NewShardWallets(pub, nil, uint32(cf.ttl), cf.shards)
	fmt.Printf("Highload relayers (ttl=%ds, pool key=%s):\n", cf.ttl, cf.keyPath)
	for i := range set {
		fmt.Printf("  shard %x: sub=%-6d %s\n", i, set[i].SubwalletID(), set[i].Address())
	}
}

func cmdDeploy(args []string) {
	cf := parseFlags(args)
	pub, priv := mustKey(cf.keyPath)
	set := highload.NewShardWallets(pub, priv, uint32(cf.ttl), cf.shards)

	fund, err := tlb.FromTON(cf.fundTON)
	if err != nil || fund.Nano().Sign() <= 0 {
		die("invalid --fund %q: expected positive GRAM", cf.fundTON)
	}

	cpub, cpriv, err := loadControlKey()
	if err != nil {
		die("control wallet: %v", err)
	}
	control := walletv5.New(cpub, cpriv, int32(cf.networkID))
	be := makeBackend(cf)

	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	fmt.Printf("Control wallet: %s\n", control.Address())
	if bal, err := be.GetBalance(ctx, control.Address()); err == nil {
		fmt.Printf("Control balance: %s TON\n", fmtTON(bal))
	}

	// Skip relayers that are already deployed so a re-run only funds the missing ones (retry-safe).
	var messages []walletv5.Message
	deployed := 0
	for i := range set {
		hl := set[i]
		if isDeployed(ctx, be, hl.Address()) {
			deployed++
			fmt.Printf("  shard %x: already deployed %s\n", i, hl.Address())
			continue
		}
		fmt.Printf("  shard %x: fund %s TON -> %s\n", i, cf.fundTON, hl.Address())
		messages = append(messages, walletv5.Message{
			Mode: uint8(1) | uint8(2), // PAY_GAS_SEPARATELY | IGNORE_ERRORS
			Internal: &tlb.InternalMessage{
				IHRDisabled: true,
				Bounce:      false, // deploying: never bounce the funding value
				DstAddr:     hl.Address(),
				Amount:      fund,
				StateInit:   hl.StateInit(),
				Body:        cell.BeginCell().EndCell(),
			},
		})
	}
	if len(messages) == 0 {
		fmt.Printf("all %d relayers already deployed — nothing to do\n", len(set))
		return
	}

	total := new(big.Int).Mul(fund.Nano(), big.NewInt(int64(len(messages))))
	fmt.Printf("Plan: deploy %d relayer(s), %s TON each = %s TON total (+fees)\n",
		len(messages), cf.fundTON, fmtTON(total))
	if cf.dry {
		fmt.Println("--dry: not broadcasting")
		return
	}

	validUntil := uint32(time.Now().Add(120 * time.Second).Unix())
	// If the control wallet isn't initialized yet (freshly funded), the first external must carry its
	// state_init to deploy it in place; otherwise a plain external is rejected.
	ctrlSeqno := uint32(0)
	ctrlDeployed := false
	if st, err := be.RunGetMethod(ctx, control.Address(), "seqno"); err == nil && len(st) > 0 {
		ctrlDeployed = true
		if s, err := be.GetSeqno(ctx, control.Address()); err == nil {
			ctrlSeqno = s
		}
	}
	body, err := control.BuildSignedBodyWithSeqno(ctrlSeqno, validUntil, messages)
	if err != nil {
		die("build deploy external: %v", err)
	}
	if ctrlDeployed {
		err = be.SendExternalMessage(ctx, control.Address(), body)
	} else {
		fmt.Println("control wallet not initialized — deploying it in the same external")
		err = be.SendExternalMessageWithInit(ctx, control.Address(), body, control.StateInit())
	}
	if err != nil {
		die("send deploy external: %v", err)
	}
	fmt.Printf("Deploy external submitted (%d relayers). Waiting for them to come up...\n", len(messages))
	waitDeployed(be, set, 120*time.Second)
}

func cmdStatus(args []string) {
	cf := parseFlags(args)
	pub, _ := mustKey(cf.keyPath)
	set := highload.NewShardWallets(pub, nil, uint32(cf.ttl), cf.shards)
	be := makeBackend(cf)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	up := 0
	for i := range set {
		hl := set[i]
		bal, _ := be.GetBalance(ctx, hl.Address())
		ok := isDeployed(ctx, be, hl.Address())
		state := "MISSING"
		if ok {
			state = "up"
			up++
		}
		fmt.Printf("  shard %x: sub=%-6d %-7s bal=%8s TON  %s\n", i, hl.SubwalletID(), state, fmtTON(bal), hl.Address())
	}
	fmt.Printf("Relayers up: %d/%d\n", up, len(set))
}

// waitDeployed polls until all relayers report deployed or the timeout elapses.
func waitDeployed(be backend.Backend, set []*highload.HighloadWallet, timeout time.Duration) {
	deadline := time.Now().Add(timeout)
	for {
		ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
		up := 0
		for i := range set {
			if isDeployed(ctx, be, set[i].Address()) {
				up++
			}
		}
		cancel()
		fmt.Printf("  deployed %d/%d\n", up, len(set))
		if up == len(set) {
			fmt.Printf("HIGHLOAD DEPLOY DONE — all %d relayers up\n", len(set))
			return
		}
		if time.Now().After(deadline) {
			fmt.Printf("timeout: only %d/%d relayers up — re-run `deploy` to fund the rest\n", up, len(set))
			os.Exit(1)
		}
		time.Sleep(5 * time.Second)
	}
}

// isDeployed returns true when the highload's get_subwallet_id get-method succeeds (uninit accounts error).
func isDeployed(ctx context.Context, be backend.Backend, addr *address.Address) bool {
	stack, err := be.RunGetMethod(ctx, addr, "get_subwallet_id")
	return err == nil && len(stack) > 0
}

// mustKey loads the POOL keypair (raw 32-byte seed) — the relayers share it, so it must already exist
// (generated by the acton deploy / genkey). We never create it here.
func mustKey(path string) (ed25519.PublicKey, ed25519.PrivateKey) {
	if _, err := os.Stat(path); err != nil {
		die("pool key %q not found: deploy the pool first (it writes pool.key), or pass --pool-key", path)
	}
	pub, priv, err := backend.LoadKeypairFromSeedFile(path)
	if err != nil {
		die("load pool key %s: %v", path, err)
	}
	return pub, priv
}

func loadControlKey() (ed25519.PublicKey, ed25519.PrivateKey, error) {
	if secret := os.Getenv("WALLET_SECRET"); secret != "" {
		return backend.LoadKeypairFromSecretHex(secret)
	}
	if m := os.Getenv("WALLET_MNEMONIC"); m != "" {
		return backend.LoadKeypairFromMnemonic(m)
	}
	return nil, nil, fmt.Errorf("set WALLET_SECRET (hex ed25519 seed/key) or WALLET_MNEMONIC")
}

func makeBackend(cf commonFlags) backend.Backend {
	if cf.endpoint != "" {
		return backend.NewHTTP(cf.endpoint, cf.apiKey)
	}
	if cf.lsConfig == "" {
		die("no backend: set --ls-config/-l or --endpoint/-e")
	}
	be, n, err := backend.NewLSFromConfigFile(cf.lsConfig)
	if err != nil {
		die("LS backend from %s: %v", cf.lsConfig, err)
	}
	fmt.Printf("LS backend: %d servers (%s)\n", n, cf.lsConfig)
	return be
}

func fmtTON(nano *big.Int) string {
	if nano == nil {
		return "0"
	}
	return tlb.FromNanoTON(nano).String()
}

func envStr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func envIntv(k string, def int) int {
	if v := os.Getenv(k); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

func atoi(s string) int {
	n, err := strconv.Atoi(s)
	if err != nil {
		die("expected integer, got %q", s)
	}
	return n
}

func die(format string, a ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", a...)
	os.Exit(1)
}
