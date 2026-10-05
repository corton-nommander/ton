package main

// Highload-V3 relayer plumbing for the gasless modes (/gassless_jetton, /gassless_gram).
//
// The relayer used to be another pool WalletSpam: it has a seqno, so relays through it serialize and the
// scheduler had to reserve BOTH wallets (pool.SendPair + RemotePendingFor). A highload-v3 has NO seqno —
// replay protection is a query_id — so any number of relays can be signed and submitted concurrently from
// one relayer. That collapses the gasless send path back to the plain single-Send pipeline (only the
// TARGET's seqno is tracked; its `sint` bumps it), and the relayer's only ceiling is its own shard. Hence
// Four relayers, one per top-two-bit shard bucket, each serving targets in its own shard.
//
// The relayers pay all the gas, so pool wallets need no GRAM of their own. They are derived from the POOL
// key + HIGHLOAD_TTL (must match what `highload deploy` used, or the addresses differ) and are deployed +
// funded out of band with `go-spam/build/highload`. The bot never deploys or tops them up: it only WARNS
// when one drops below HIGHLOAD_MIN_BALANCE (default 1000 GRAM), at run start and on every status tick.

import (
	"context"
	"fmt"
	"math/big"
	"strings"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/highload"
	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
)

// highloadBalanceEvery is how often the background watcher re-reads all four relayer balances. Four RPCs per
// interval is negligible next to the send rate, and it runs off the send path so a slow backend can't
// stall the loop.
const highloadBalanceEvery = 30 * time.Second

// hlBalances is one snapshot of all four relayer balances, published by the watcher goroutine and read
// (never computed) on the status path.
type hlBalances struct {
	nano []*big.Int // nil => that relayer's balance RPC failed
	low  []int      // shard buckets below the warn threshold, ascending
	errs int        // how many balance reads failed
	at   time.Time
}

// warnLine renders the low-balance warning appended to status messages, or "" when everything is funded
// (and when no snapshot has been taken yet). Unreadable relayers are surfaced too — an undeployed or
// unreachable relayer silently drops every relay routed to its shard, which looks like a 1/4 failure rate.
func (s *hlBalances) warnLine(threshold *big.Int) string {
	if s == nil || (len(s.low) == 0 && s.errs == 0) {
		return ""
	}
	var sb strings.Builder
	sb.WriteString("\n<b>⚠️ highload relayers low</b>\n<code>")
	for _, i := range s.low {
		fmt.Fprintf(&sb, "shard %x: %s TON\n", i, fmtTONNano(s.nano[i]))
	}
	if s.errs > 0 {
		fmt.Fprintf(&sb, "%d relayer(s) unreadable\n", s.errs)
	}
	fmt.Fprintf(&sb, "threshold %s TON — top up manually", fmtTONNano(threshold))
	sb.WriteString("</code>")
	return sb.String()
}

// min returns the smallest successfully-read relayer balance, or nil when none could be read. It's the
// one number that answers "is the relay fleet still funded".
func (s *hlBalances) min() *big.Int {
	var m *big.Int
	for _, v := range s.nano {
		if v != nil && (m == nil || v.Cmp(m) < 0) {
			m = v
		}
	}
	return m
}

func fmtTONNano(nano *big.Int) string {
	if nano == nil {
		return "?"
	}
	return tlb.FromNanoTON(nano).String()
}

// readHighloadBalances reads all four relayer balances and classifies them against the warn threshold.
// Fail-soft: a failed read leaves that slot nil and bumps errs — it is NOT counted as low, so an RPC blip
// can't read as "out of funds".
func (r *Runtime) readHighloadBalances(ctx context.Context) *hlBalances {
	set := r.initHighloads()
	threshold := big.NewInt(r.highloadMinBalance.Load())
	snap := &hlBalances{at: time.Now(), nano: make([]*big.Int, len(set))}
	be := r.Backend()
	for i := range set {
		bal, err := be.GetBalance(ctx, set[i].Address())
		if err != nil {
			snap.errs++
			continue
		}
		snap.nano[i] = bal
		if bal.Cmp(threshold) < 0 {
			snap.low = append(snap.low, i)
		}
	}
	return snap
}

// watchHighloadBalances publishes a fresh snapshot every highloadBalanceEvery until ctx is done, starting
// with an immediate read so an underfunded relayer is reported at run start rather than 30s in. It runs
// OFF the send path, so the four RPCs never delay a send; the status tick just reads the latest snapshot.
// A relayer that newly crosses below the threshold is logged and audited ONCE per crossing (re-armed when
// it is topped back up), so the audit chat gets an alert instead of a warning every 30s.
func (r *Runtime) watchHighloadBalances(ctx context.Context, kind string) {
	warned := map[int]bool{}
	tick := time.NewTicker(highloadBalanceEvery)
	defer tick.Stop()
	for {
		readCtx, cancel := context.WithTimeout(ctx, 20*time.Second)
		snap := r.readHighloadBalances(readCtx)
		cancel()
		r.hlBal.Store(snap)

		threshold := big.NewInt(r.highloadMinBalance.Load())
		lowNow := map[int]bool{}
		for _, i := range snap.low {
			lowNow[i] = true
			if !warned[i] {
				warned[i] = true
				r.logger.Printf("[WARN] %s: highload relayer shard %x low: %s TON (threshold %s TON) %s",
					kind, i, fmtTONNano(snap.nano[i]), fmtTONNano(threshold), r.initHighloads()[i].Address())
				r.audit("⚠️ highload relayer shard %x low: %s TON\naddress: %s\nthreshold: %s TON — top up manually",
					i, fmtTONNano(snap.nano[i]), r.initHighloads()[i].Address(), fmtTONNano(threshold))
			}
		}
		for i := range warned {
			if !lowNow[i] {
				delete(warned, i)
			}
		}

		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}
	}
}

func (r *Runtime) newGaslessBatcher(ctx context.Context, set []*highload.HighloadWallet, stats *pool.Stats) *highload.Batcher {
	return highload.NewBatcher(ctx, set, func(ctx context.Context, addr *address.Address, body *cell.Cell) error {
		return r.Backend().SendExternalMessage(ctx, addr, body)
	}, highload.BatchOptions{
		Limiter: &r.batchLimiter, SendTimeout: r.SendTimeout, NoRetry: r.noRetry.Load,
		OnDispatch: func(dst *address.Address, ext *cell.Cell, requests []context.Context) {
			for _, ctx := range requests {
				r.bindObserved(ctx, dst, ext)
			}
		},
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
}
