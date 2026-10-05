package main

// /hamster_kombat — the same pool-jetton-spam as /jetton, but over the FULL 2.5M-wallet pool instead of
// a sub-pool window. Every wallet in [0, poolTotal) signs its own external carrying one op::transfer of
// 1 jetton (from its pre-deployed jetton wallet) to a RANDOM OTHER wallet in the same pool — the Hamster
// Kombat load simulation. Same on-chain wallets as every other mode (one pool, one key/minter/jw-code);
// the difference is only the RANGE: this one drives all of [0, poolTotal) while /jetton and friends drive
// one /pool_size sub-pool. All pool-backed modes share r.poolRun, so they can never run at once and race
// seqnos on the wallets their ranges overlap on.

import (
	"context"
	"fmt"
	mathrand "math/rand"
	"time"

	tg "github.com/go-telegram/bot"
	"github.com/go-telegram/bot/models"

	"github.com/victor/jetton-bomb-go-spam/pkg/jetton"
	"github.com/victor/jetton-bomb-go-spam/pkg/pool"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

// buildHamsterExternalForEntry mirrors buildJettonExternalForEntry but draws the recipient from the WHOLE
// pool [0, poolTotal) instead of the run's sub-pool window. Same shared pkg/jetton builder, so the
// externals stay byte-identical to /jetton and cmd/spam.
func (r *Runtime) buildHamsterExternalForEntry(jWalletCode *cell.Cell, e *pool.Entry, seqno uint32, chosen ...*address.Address) (*cell.Cell, error) {
	var dest *address.Address
	if len(chosen) > 0 {
		dest = chosen[0]
	}
	if dest == nil {
		dest = r.initSpamPool().Get(mathrand.Intn(r.poolTotal)).Wallet.Address()
	}
	validUntil := uint32(time.Now().Unix() + 120)
	return jetton.BuildTransferExternal(
		e.Wallet,
		seqno,
		jWalletCode,
		r.jettonMinter,
		r.jettonValue,
		r.jettonInitMode.Load(),
		dest,
		mathrand.Intn(1_000_000),
		validUntil,
	)
}

// handleHamster runs jetton spam over the full 2.5M pool. Modeled on handleJetton, minus the
// /check_balance drain gate (the 2.5M wallets are funded ~1 TON each — no drain tracking needed).
func (r *Runtime) handleHamster(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /hamster_kombat user=%s text=%q", user, msgText(update.Message))

	if active := r.poolRun.Load(); active != nil {
		reply(ctx, b, chat, fmt.Sprintf("Already running: %s. sent=%d, failed=%d", active.kind, active.stats.Sent.Load(), active.stats.Failed.Load()))
		return
	}
	freqArg := argAt(msgText(update.Message), 1)
	if freqArg == "" {
		reply(ctx, b, chat, "Usage: /hamster_kombat <freq>")
		return
	}
	freq, ok := parsePositiveInt(freqArg)
	if !ok {
		reply(ctx, b, chat, "freq must be a positive integer. Usage: /hamster_kombat <freq>")
		return
	}
	periodMs := time.Duration(1000/freq) * time.Millisecond

	p := r.initSpamPool()
	reply(ctx, b, chat, fmt.Sprintf("Starting Hamster Kombat jetton spam\nWallets: %d\nFreq: %d req/s (%dms)", r.poolTotal, freq, periodMs/time.Millisecond))

	jwalletCode := r.jwCode
	stats := &pool.Stats{}
	errs := NewErrStack()
	loopCtx, cancel := context.WithCancel(context.Background())
	run := &SpamRun{cancel: cancel, stats: stats, freq: freq, period: periodMs, startedAt: time.Now(), kind: "hamster", errs: errs}
	if !r.poolRun.CompareAndSwap(nil, run) {
		cancel()
		reply(ctx, b, chat, "Already running")
		return
	}

	cfg := pool.LoopConfig{
		Rate:        float64(freq),
		MaxInflight: int64(freq*2 + 10),
		SendTimeout: r.SendTimeout,
		ErrSink:     errs.Record,
		StatsEvery:  uint64(freq) * 30,
		NoRetry:     r.noRetry.Load,
		// The 2.5M pool's epoch is tens of minutes long, so a warm cache would rarely be re-hit anyway;
		// fetch the seqno fresh per send instead of caching last+1.
		NoSeqnoCache: true,
		Send: func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			var chosen *address.Address
			if v := observedFrom(ctx); v != nil {
				chosen = v.to
			}
			body, err := r.buildHamsterExternalForEntry(jwalletCode, e, seqno, chosen)
			if err != nil {
				return err
			}
			r.bindObserved(ctx, e.Wallet.Address(), body)
			return r.Backend().SendExternalMessage(ctx, e.Wallet.Address(), body)
		},
		FetchSeqno: func(ctx context.Context, e *pool.Entry) (uint32, error) {
			return r.Backend().GetSeqno(ctx, e.Wallet.Address())
		},
	}
	r.watchTransfers(&cfg, run, func(*pool.Entry) *address.Address {
		return p.Get(mathrand.Intn(r.poolTotal)).Wallet.Address()
	})
	tickStart := time.Now()
	var lastSnap Snap
	cfg.OnTick = func(s *pool.Stats) {
		periodSec := time.Since(tickStart).Seconds()
		topErrs, totalErrs := errs.SnapshotDelta(10)
		WriteSnapshot(r.errLog, "hamster", topErrs, totalErrs, periodSec)
		text := formatStatusHTML("hamster", s, lastSnap, periodSec)
		text += r.streamLine(run)
		if r.ErrorsEnabled() {
			text += FormatBreakdownHTML(topErrs, totalErrs, periodSec)
		}
		r.sendStatus(loopCtx, b, chat, text)
		r.auditEdit(run.auditMsgID, r.auditTransferStatus(run, s, time.Since(run.startedAt).Seconds()))
		tickStart = time.Now()
		lastSnap = takeSnap(s)
	}

	run.auditHeader = fmt.Sprintf("start hamster_kombat spam\nby: %s\nfreq: %d req/s\ninit mode: %d\nbackend: %s",
		user, freq, r.jettonInitMode.Load(), r.Backend().Description())
	run.auditMsgID = r.auditMessage(r.auditTransferStatus(run, stats, 0))

	go func() {
		defer r.poolRun.CompareAndSwap(run, nil)
		p.Run(loopCtx, cfg, stats)
		r.logger.Printf("[INFO] hamster loop finished sent=%d failed=%d", stats.Sent.Load(), stats.Failed.Load())
	}()
}

// /hamster_kombat_stop
func (r *Runtime) handleHamsterStop(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /hamster_kombat_stop user=%s", user)
	run := r.poolRun.Load()
	if run == nil || run.kind != "hamster" {
		reply(ctx, b, chat, "Hamster Kombat spam is not running")
		return
	}
	run.cancel()
	elapsed := time.Since(run.startedAt).Seconds()
	sent := run.stats.Sent.Load()
	failed := run.stats.Failed.Load()
	avg := 0.0
	if elapsed > 0 {
		avg = float64(sent) / elapsed
	}
	r.audit("stop hamster_kombat spam\nby: %s\nsent %d, failed %d, %.0f/s, %.0fs",
		user, sent, failed, avg, elapsed)
	reply(ctx, b, chat, fmt.Sprintf(
		"Stopping Hamster Kombat spam...\nSent: %d, Failed: %d\nElapsed: %.1fs, Avg: %.1f sent/s",
		sent, failed, elapsed, avg))
}
