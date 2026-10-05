package main

import (
	"context"
	"fmt"
	"log"
	mathrand "math/rand"
	"os"
	"path/filepath"
	"sync/atomic"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
	"github.com/victor/jetton-bomb-go-spam/pkg/fats"
	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

// w5RunOpts carries everything the zerostate spam modes (zerostate / zerostate-fats) need beyond the
// shared loop knobs. Both send FROM the generated wallet-v5 wallets; fatsTarget picks the recipient.
type w5RunOpts struct {
	fatsTarget   bool      // recipient is a fat load-target (zerostate-fats) vs another W5 (zerostate)
	fatsFile     string    // fats.addrs (raw wc:hex recipients); default: <manifest dir>/fats.addrs (fats target)
	manifestPath string    // state-gen manifest.json (source of seed / minter / num_v5 / wallet_id)
	seedHex      string    // override manifest seed_hex
	minterStr    string    // override manifest minter (user-friendly or raw address)
	count        int       // override manifest num_v5 (sender W5 index space)
	value        tlb.Coins // GRAM attached per transfer
	initMode     int32     // recipient internal_init_mode (fats target: 0; zerostate: --init-mode, default 0)
	forwardTON   uint64    // nanoTON forwarded so a transfer_notification reaches the fat (fats target)
	mutate       bool      // set the fat mutate flag in the forward payload (fats target)
	sequential   bool      // walk fats.addrs in order instead of random (fats target)
	rate         float64
	maxInflight  int
	sendTimeout  time.Duration
	noRetry      bool
}

// runW5Spam drives jetton transfers FROM the deterministically-generated wallet-v5 wallets. With fatsTarget
// set (zerostate-fats) the recipients are the fat load-target accounts (real op::transfer with
// forward_ton>0, init mode 0, so the notification lands ON the fat); otherwise (zerostate) the recipient is
// a random OTHER generated W5 wallet (no forward; init mode from --init-mode, default 0 — the realistic
// mainnet load. Recipients are already deployed, so bare/1 would also work but is not the default). It is
// fully self-contained (its own W5 sender pool + driver), so it never touches the WalletSpam pool.
func runW5Spam(ctx context.Context, be backend.Backend, o w5RunOpts, spamLog, errLog *log.Logger, startedAt time.Time) {
	var (
		seed     [32]byte
		minter   *address.Address
		count    = o.count
		walletID int32
		fatsFile = o.fatsFile
	)
	modeName := "zerostate"
	if o.fatsTarget {
		modeName = "zerostate-fats"
	}

	if o.manifestPath != "" {
		m, err := fats.LoadManifest(o.manifestPath)
		if err != nil {
			die("%v", err)
		}
		if seed, err = m.Seed(); err != nil {
			die("%v", err)
		}
		if minter, err = m.Minter(); err != nil {
			die("%v", err)
		}
		if count <= 0 {
			count = int(m.NumV5)
		}
		walletID = m.WalletID
		if o.fatsTarget && fatsFile == "" {
			fatsFile = filepath.Join(filepath.Dir(o.manifestPath), "fats.addrs")
		}
	}

	// Overrides win over the manifest.
	if o.seedHex != "" {
		s, err := (&fats.Manifest{SeedHex: o.seedHex}).Seed()
		if err != nil {
			die("%v", err)
		}
		seed = s
	}
	if o.minterStr != "" {
		mAddr, err := address.ParseAddr(o.minterStr)
		if err != nil {
			die("invalid --minter %q: %v", o.minterStr, err)
		}
		minter = mAddr
	}

	if count <= 0 {
		die("--mode %s needs the wallet count (--count or manifest num_v5)", modeName)
	}
	if minter == nil {
		die("--mode %s needs the minter (--minter or manifest minter_addr_hex)", modeName)
	}

	jwCode := walletspam.LoadJettonWalletCode()
	wp := fats.NewWalletPool(count, seed, walletID)
	stats := &pool.Stats{}

	// Recipient selection + transfer params. zerostate-fats sends to the fat targets (init mode 0, forward
	// > 0); zerostate sends to another random W5 (init mode = --init-mode, default 0, forward 0, no mutate).
	var sendFn func(ctx context.Context, e *fats.Entry, seqno uint32) error
	if o.fatsTarget {
		if fatsFile == "" {
			die("--mode zerostate-fats needs --fats-file <fats.addrs> (or --manifest to locate it)")
		}
		recipients, err := loadTargets(fatsFile) // same raw wc:hex format as --mode targets
		if err != nil {
			die("%v", err)
		}
		order := "random"
		if o.sequential {
			order = "sequential"
		}
		fmt.Printf("Mode: zerostate-fats — W5[0,%d) -> jetton transfer to %d fat targets (%s)\n", count, len(recipients), fatsFile)
		fmt.Printf("  value=%s GRAM, forward=%.4f GRAM, mutate=%t, order=%s\n",
			o.value.String(), float64(o.forwardTON)/1e9, o.mutate, order)
		var cursor uint64
		sendFn = func(ctx context.Context, e *fats.Entry, seqno uint32) error {
			idx := mathrand.Intn(len(recipients))
			if o.sequential {
				idx = int((atomic.AddUint64(&cursor, 1) - 1) % uint64(len(recipients)))
			}
			target := recipients[idx]
			validUntil := uint32(time.Now().Unix() + 120)
			body, berr := fats.BuildW5JettonTransfer(e.W5, seqno, jwCode, minter, o.value, o.initMode, target, o.forwardTON, o.mutate, false, validUntil)
			if berr != nil {
				spamLog.Printf("fats w5=%s seqno=%d -> %s build FAIL: %v", e.W5.Address().String(), seqno, target.String(), berr)
				return berr
			}
			return be.SendExternalMessage(ctx, e.W5.Address(), body)
		}
	} else {
		fmt.Printf("Mode: zerostate — W5[0,%d) -> cheap jetton transfer to a random OTHER generated W5 wallet\n", count)
		fmt.Printf("  value=%s GRAM, init-mode=%d, forward=0\n", o.value.String(), o.initMode)
		sendFn = func(ctx context.Context, e *fats.Entry, seqno uint32) error {
			// Recipient owner = another random generated W5 (its jetton wallet is pre-deployed in the
			// zerostate). Pick a different index than the sender when possible.
			recipientOwner := wp.Get(mathrand.Intn(count)).W5.Address()
			for count > 1 && recipientOwner.Equals(e.W5.Address()) {
				recipientOwner = wp.Get(mathrand.Intn(count)).W5.Address()
			}
			validUntil := uint32(time.Now().Unix() + 120)
			body, berr := fats.BuildW5JettonTransfer(e.W5, seqno, jwCode, minter, o.value, o.initMode, recipientOwner, 0, false, true, validUntil)
			if berr != nil {
				spamLog.Printf("zerostate w5=%s seqno=%d -> %s build FAIL: %v", e.W5.Address().String(), seqno, recipientOwner.String(), berr)
				return berr
			}
			return be.SendExternalMessage(ctx, e.W5.Address(), body)
		}
	}

	fmt.Printf("  seed=%x wallet_id=%d minter=%s\n", seed, walletID, minter.String())
	fmt.Printf("Starting %s loop...\n", modeName)

	sendTimeout := o.sendTimeout
	cfg := fats.LoopConfig{
		Rate:        o.rate,
		MaxInflight: int64(o.maxInflight),
		SendTimeout: func() time.Duration { return sendTimeout },
		ErrLog:      errLog,
		StatsEvery:  1000,
		NoRetry:     func() bool { return o.noRetry },
		Send:        sendFn,
		FetchSeqno: func(ctx context.Context, e *fats.Entry) (uint32, error) {
			return be.GetSeqno(ctx, e.W5.Address())
		},
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

	go func() {
		<-ctx.Done()
		printSummary(startedAt, stats, spamLog, errLog)
		os.Exit(0)
	}()

	wp.Run(ctx, cfg, stats)
	printSummary(startedAt, stats, spamLog, errLog)
}
