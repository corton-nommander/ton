package main

import (
	"context"
	"fmt"
	"math/big"
	mathrand "math/rand"
	"time"

	tg "github.com/go-telegram/bot"
	"github.com/go-telegram/bot/models"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"

	"github.com/victor/jetton-bomb-go-spam/pkg/gram"
	"github.com/victor/jetton-bomb-go-spam/pkg/highload"
	"github.com/victor/jetton-bomb-go-spam/pkg/jetton"
	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
)

const (
	modeGram           = "gram"
	modeGasslessGram   = "gassless_gram"
	modeGasslessJetton = "gassless_jetton"
	gaslessValidity    = 120 * time.Second
)

func (r *Runtime) handleGram(ctx context.Context, b *tg.Bot, update *models.Update) {
	r.handleTransferSpam(ctx, b, update, modeGram)
}

func (r *Runtime) handleGasslessGram(ctx context.Context, b *tg.Bot, update *models.Update) {
	r.handleTransferSpam(ctx, b, update, modeGasslessGram)
}

func (r *Runtime) handleGasslessJetton(ctx context.Context, b *tg.Bot, update *models.Update) {
	r.handleTransferSpam(ctx, b, update, modeGasslessJetton)
}

func (r *Runtime) handleGramStop(ctx context.Context, b *tg.Bot, update *models.Update) {
	r.handleTransferSpamStop(ctx, b, update, modeGram)
}

func (r *Runtime) handleGasslessGramStop(ctx context.Context, b *tg.Bot, update *models.Update) {
	r.handleTransferSpamStop(ctx, b, update, modeGasslessGram)
}

func (r *Runtime) handleGasslessJettonStop(ctx context.Context, b *tg.Bot, update *models.Update) {
	r.handleTransferSpamStop(ctx, b, update, modeGasslessJetton)
}

func (r *Runtime) handleTransferSpam(
	ctx context.Context,
	b *tg.Bot,
	update *models.Update,
	kind string,
) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /%s user=%s text=%q", kind, user, msgText(update.Message))

	if active := r.poolRun.Load(); active != nil {
		reply(ctx, b, chat, fmt.Sprintf(
			"Already running: %s. sent=%d, failed=%d",
			active.kind,
			active.stats.Sent.Load(),
			active.stats.Failed.Load(),
		))
		return
	}
	if r.poolTotal < 2 {
		reply(ctx, b, chat, "POOL_TOTAL must be at least 2 for pool transfers")
		return
	}
	freqArg := argAt(msgText(update.Message), 1)
	if freqArg == "" {
		reply(ctx, b, chat, fmt.Sprintf("Usage: /%s <freq>", kind))
		return
	}
	freq, ok := parsePositiveInt(freqArg)
	if !ok {
		reply(ctx, b, chat, fmt.Sprintf("freq must be a positive integer. Usage: /%s <freq>", kind))
		return
	}

	// Senders: every pool-backed mode here, fat modes included, drives ONE randomly-chosen sub-pool window
	// of /pool_size wallets. To drive the whole pool instead, set /pool_size to the deployed total — that
	// leaves a single sub-pool, so the window IS the pool.
	fat := kind == modeFatGram || kind == modeFatJetton
	p := r.initSpamPool()
	poolStart, senderCount := r.pickSubPool()
	// The gasless modes are relayed by the highload set (which pays the gas); plain /gram sends straight
	// from the pool wallet and needs no relayer.
	gasless := kind == modeGasslessGram || kind == modeGasslessJetton
	var relayers []*highload.HighloadWallet
	if gasless {
		relayers = r.initHighloads()
	}
	// Derive the fat tier now (up to ~1s for 431k accounts) so the run either starts with a full recipient
	// set or not at all.
	var fatAddrs []*address.Address
	fatSize := int(r.fatSize.Load())
	if fat {
		all, err := r.fatTargets(fatSize)
		if err != nil {
			reply(ctx, b, chat, fmt.Sprintf("fat targets: %v", err))
			return
		}
		// Recipients get the same fixed-slice treatment as the senders: /fat_count cuts the tier into
		// aligned slices and this run draws one. Which slice is log-only — the operator tunes the COUNT,
		// and printing a range of derived addresses in chat would just be noise.
		fatStart, fatCount := r.pickFatWindow(len(all))
		fatAddrs = all[fatStart : fatStart+fatCount]
		r.logger.Printf("[INFO] %s targets: %d fat accounts of %dKB, window [%d, %d) of %d",
			kind, fatCount, fatSize, fatStart, fatStart+fatCount, len(all))
	}
	stats := &pool.Stats{}
	errs := NewErrStack()
	loopCtx, cancel := context.WithCancel(context.Background())
	period := time.Duration(1000/freq) * time.Millisecond
	run := &SpamRun{
		cancel: cancel, stats: stats, freq: freq, period: period,
		startedAt: time.Now(), kind: kind, errs: errs,
	}
	if !r.poolRun.CompareAndSwap(nil, run) {
		cancel()
		reply(ctx, b, chat, "Another pool spam mode started concurrently")
		return
	}

	cfg := pool.LoopConfig{
		Rate:        float64(freq),
		MaxInflight: int64(freq*2 + 10),
		SendTimeout: r.SendTimeout,
		ErrSink:     errs.Record,
		StatsEvery:  uint64(freq) * 30,
		NoRetry:     r.noRetry.Load,
		WindowStart: poolStart,
		WindowCount: senderCount,
		FetchSeqno: func(ctx context.Context, e *pool.Entry) (uint32, error) {
			return r.Backend().GetSeqno(ctx, e.Wallet.Address())
		},
	}
	var batcher *highload.Batcher
	if gasless {
		batcher = r.newGaslessBatcher(loopCtx, relayers, stats)
		cfg.BatchedSend = true
		cfg.RemotePendingFor = highload.BatchValidity + 10*time.Second
		cfg.SendTimeout = func() time.Duration { return highload.RequestTimeout(r.SendTimeout()) }
	}

	switch kind {
	case modeGram:
		cfg.Send = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			recipient := destinationFor(ctx, func() *address.Address {
				return p.Get(poolStart + randomOtherWallet(senderCount, int(e.Wallet.SubwalletID)-poolStart)).Wallet.Address()
			})
			body, err := gram.BuildTransferExternal(
				e.Wallet,
				seqno,
				uint32(time.Now().Add(gaslessValidity).Unix()),
				recipient,
				r.gramValue,
			)
			if err != nil {
				return err
			}
			r.bindObserved(ctx, e.Wallet.Address(), body)
			return r.Backend().SendExternalMessage(ctx, e.Wallet.Address(), body)
		}

	// Both gasless modes are batched by the four Highload-V3 relayers (see highload.go), NOT by another pool
	// WalletSpam. The highload has no seqno, so there is no relayer to reserve: the plain single-Send path
	// is enough and only the TARGET's seqno is tracked (its `sint` bumps it, exactly as its own external
	// would). The relayer pays the gas of both hops, so the pool wallet needs no GRAM of its own.
	case modeGasslessGram:
		relayValue := r.gramRelayValue()
		cfg.Send = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			recipient := destinationFor(ctx, func() *address.Address {
				return p.Get(poolStart + randomOtherWallet(senderCount, int(e.Wallet.SubwalletID)-poolStart)).Wallet.Address()
			})
			actionMessage, err := gram.BuildTransferInternal(recipient, r.gramValue)
			if err != nil {
				return err
			}
			return batcher.Submit(ctx, e.Wallet, seqno, relayValue, actionMessage)
		}

	case modeGasslessJetton:
		relayValue := r.relayValue()
		cfg.Send = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			recipient := destinationFor(ctx, func() *address.Address {
				return p.Get(poolStart + randomOtherWallet(senderCount, int(e.Wallet.SubwalletID)-poolStart)).Wallet.Address()
			})
			actionMessage, err := jetton.BuildTransferInternal(
				e.Wallet.Address(),
				r.jwCode,
				r.jettonMinter,
				r.gasslessJettonValue,
				r.jettonInitMode.Load(),
				recipient,
				e.Wallet.Address(),
				mathrand.Intn(1_000_000),
			)
			if err != nil {
				return err
			}
			return batcher.Submit(ctx, e.Wallet, seqno, relayValue, actionMessage)
		}

	// Fat modes: same pool-wallet-signed external as /gram and /jetton, but the recipient is a big-state
	// fat account, so collating the message forces a walk of its storage dict.
	case modeFatGram:
		cfg.Send = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			target := destinationFor(ctx, func() *address.Address { return fatAddrs[mathrand.Intn(len(fatAddrs))] })
			body, err := gram.BuildTransferExternal(
				e.Wallet, seqno, uint32(time.Now().Add(gaslessValidity).Unix()), target, r.gramValue)
			if err != nil {
				return err
			}
			r.bindObserved(ctx, e.Wallet.Address(), body)
			return r.Backend().SendExternalMessage(ctx, e.Wallet.Address(), body)
		}

	case modeFatJetton:
		cfg.Send = func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			target := destinationFor(ctx, func() *address.Address { return fatAddrs[mathrand.Intn(len(fatAddrs))] })
			// initMode 0 is forced, not taken from /initmode: the fat's jetton wallet is a stranger's and
			// is NOT pre-deployed, so the transfer must carry its full state_init. forwardTON > 0 is what
			// makes a transfer_notification land ON the fat — without it only the jetton wallet is touched
			// and the fat's big state is never read.
			body, err := jetton.BuildFatTransferExternal(
				e.Wallet, seqno, r.jwCode, r.jettonMinter, r.fatJettonValue, 0,
				target, r.fatForwardTON, r.fatMutate.Load(), uint32(time.Now().Add(gaslessValidity).Unix()))
			if err != nil {
				return err
			}
			r.bindObserved(ctx, e.Wallet.Address(), body)
			return r.Backend().SendExternalMessage(ctx, e.Wallet.Address(), body)
		}
	}
	r.watchTransfers(&cfg, run, func(e *pool.Entry) *address.Address {
		if fat {
			return fatAddrs[mathrand.Intn(len(fatAddrs))]
		}
		return p.Get(poolStart + randomOtherWallet(senderCount, int(e.Wallet.SubwalletID)-poolStart)).Wallet.Address()
	})

	// Relayer balance watcher: publishes snapshots to r.hlBal off the send path, so the status tick can
	// warn about underfunded relayers without spending four RPCs inline. No auto-topup — funding is manual.
	if gasless {
		go r.watchHighloadBalances(loopCtx, kind)
	}

	tickStart := time.Now()
	var lastSnap Snap
	cfg.OnTick = func(s *pool.Stats) {
		periodSec := time.Since(tickStart).Seconds()
		topErrs, totalErrs := errs.SnapshotDelta(10)
		WriteSnapshot(r.errLog, kind, topErrs, totalErrs, periodSec)
		text := formatStatusHTML(kind, s, lastSnap, periodSec)
		text += r.streamLine(run)
		if gasless {
			text += r.hlBal.Load().warnLine(big.NewInt(r.highloadMinBalance.Load()))
		}
		if r.ErrorsEnabled() {
			text += FormatBreakdownHTML(topErrs, totalErrs, periodSec)
		}
		r.sendStatus(loopCtx, b, chat, text)
		r.auditEdit(run.auditMsgID, r.auditTransferStatus(run, s, time.Since(run.startedAt).Seconds()))
		tickStart = time.Now()
		lastSnap = takeSnap(s)
	}

	details := fmt.Sprintf("value: %s GRAM", r.gramValue.String())
	if kind == modeGasslessJetton {
		details = fmt.Sprintf(
			"jetton value: %s GRAM\nrelay value: %s GRAM\ninit mode: %d",
			r.gasslessJettonValue.String(),
			r.relayValue().String(),
			r.jettonInitMode.Load(),
		)
	} else if kind == modeGasslessGram {
		details += fmt.Sprintf("\nrelay value: %s GRAM", r.gramRelayValue().String())
	}
	if gasless {
		details += fmt.Sprintf("\nrelayers: %d highload-v3 (ttl %ds, pay all gas)\nbatches: 500ms, self-message, max 254 / 60 KiB", len(relayers), r.highloadTTL)
	}
	if kind == modeFatGram {
		details = fmt.Sprintf("value: %s GRAM\ntargets: %d fat accounts of %d KB",
			r.gramValue.String(), len(fatAddrs), fatSize)
	} else if kind == modeFatJetton {
		details = fmt.Sprintf("jetton value: %s GRAM\nforward: %s GRAM\nmutate: %t\ninit mode: 0 (forced)\ntargets: %d fat accounts of %d KB",
			r.fatJettonValue.String(), tlb.FromNanoTONU(r.fatForwardTON).String(), r.fatMutate.Load(), len(fatAddrs), fatSize)
	}
	run.auditHeader = fmt.Sprintf(
		"start %s spam\nby: %s\nfreq: %d req/s\n%s\nwallets: %d %s\nbackend: %s",
		kind,
		user,
		freq,
		details,
		senderCount,
		poolRangeText(poolStart, senderCount, r.poolTotal),
		r.Backend().Description(),
	)
	run.auditMsgID = r.auditMessage(r.auditTransferStatus(run, stats, 0))
	replyHTML(ctx, b, chat, fmt.Sprintf(
		"Starting %s spam\nWallets: %s\nFreq: %d req/s\n%s",
		kind,
		r.poolWindowLine(p, poolStart, senderCount),
		freq,
		details,
	))

	go func() {
		defer r.poolRun.CompareAndSwap(run, nil)
		if batcher != nil {
			defer batcher.Close()
		}
		p.Run(loopCtx, cfg, stats)
		r.logger.Printf(
			"[INFO] %s loop finished sent=%d failed=%d",
			kind,
			stats.Sent.Load(),
			stats.Failed.Load(),
		)
	}()
}

func (r *Runtime) handleTransferSpamStop(
	ctx context.Context,
	b *tg.Bot,
	update *models.Update,
	kind string,
) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /%s_stop user=%s", kind, user)
	run := r.poolRun.Load()
	if run == nil || run.kind != kind {
		reply(ctx, b, chat, fmt.Sprintf("%s spam is not running", kind))
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
	r.audit(
		"stop %s spam\nby: %s\nsent %d, failed %d, %.0f/s, %.0fs",
		kind,
		user,
		sent,
		failed,
		avg,
		elapsed,
	)
	reply(ctx, b, chat, fmt.Sprintf(
		"Stopping %s spam...\nSubmitted: %d, Failed: %d\nElapsed: %.1fs, Avg: %.1f/s",
		kind,
		sent,
		failed,
		elapsed,
		avg,
	))
}

func randomOtherWallet(count int, excluded int) int {
	id := mathrand.Intn(count - 1)
	if id >= excluded {
		id++
	}
	return id
}
