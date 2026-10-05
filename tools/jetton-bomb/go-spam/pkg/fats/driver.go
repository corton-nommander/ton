package fats

import (
	"context"
	"log"
	"math/rand"
	"sync"
	"sync/atomic"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletv5"
)

// Entry is one deterministically-derived wallet-v5 sender with a mutex-protected seqno cache. It mirrors
// pool.Entry but holds a *walletv5.Wallet (the fat spammer sends FROM the generated W5 wallets, not from
// the WalletSpam pool), so it needs its own tiny driver rather than reusing pool.Pool.
type Entry struct {
	W5    *walletv5.Wallet
	mu    sync.Mutex
	seqno uint32
	valid bool
}

// SendFunc / FetchSeqno match pool's, over *fats.Entry.
type SendFunc func(ctx context.Context, e *Entry, seqno uint32) error
type FetchSeqno func(ctx context.Context, e *Entry) (uint32, error)

// WalletPool is the [0,count) space of generated wallet-v5 wallets, derived lazily from the shared seed.
type WalletPool struct {
	count    int
	seed     [32]byte
	walletID int32

	mu      sync.RWMutex
	entries map[int]*Entry
}

// NewWalletPool builds the sender pool over wallet-v5 indices [0,count). seed + walletID must match the
// state generator (manifest seed_hex + wallet_id) so the derived addresses are the ones in the zerostate.
func NewWalletPool(count int, seed [32]byte, walletID int32) *WalletPool {
	return &WalletPool{count: count, seed: seed, walletID: walletID, entries: make(map[int]*Entry, count)}
}

func (p *WalletPool) Count() int { return p.count }

// Get returns the Entry for sender index id, deriving its wallet-v5 lazily.
func (p *WalletPool) Get(id int) *Entry {
	p.mu.RLock()
	e, ok := p.entries[id]
	p.mu.RUnlock()
	if ok {
		return e
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if e, ok := p.entries[id]; ok {
		return e
	}
	e = &Entry{W5: walletv5.DeriveW5(p.seed, uint64(id), p.walletID)}
	p.entries[id] = e
	return e
}

// LoopConfig mirrors pool.LoopConfig (over *fats.Entry). See pkg/pool for the field semantics.
type LoopConfig struct {
	Rate        float64
	MaxInflight int64
	SendTimeout func() time.Duration
	ErrLog      *log.Logger
	ErrSink     func(phase string, errMsg string)
	OnTick      func(s *pool.Stats)
	StatsEvery  uint64
	NoRetry     func() bool
	Gate        func(ctx context.Context, e *Entry) bool
	Send        SendFunc
	FetchSeqno  FetchSeqno
}

// Run drives the shuffle-once token-bucket loop until ctx is cancelled. A faithful copy of pool.Run's
// mechanics (the WalletSpam pool is concrete, so the loop can't be shared) reusing pool.Stats.
func (p *WalletPool) Run(ctx context.Context, cfg LoopConfig, stats *pool.Stats) {
	if cfg.Rate <= 0 {
		return
	}
	maxOwed := cfg.Rate
	if maxOwed < 1 {
		maxOwed = 1
	}
	last := time.Now()
	owed := 0.0
	var lastReportTotal uint64

	perm := rand.Perm(p.count)
	cursor := 0

	for {
		select {
		case <-ctx.Done():
			return
		default:
		}
		now := time.Now()
		owed += now.Sub(last).Seconds() * cfg.Rate
		last = now
		if owed > maxOwed {
			owed = maxOwed
		}
		for owed >= 1 {
			owed -= 1
			if stats.Inflight.Load() >= cfg.MaxInflight {
				stats.Throttled.Add(1)
				continue
			}
			id := perm[cursor]
			cursor++
			if cursor >= len(perm) {
				rand.Shuffle(len(perm), func(i, j int) { perm[i], perm[j] = perm[j], perm[i] })
				cursor = 0
			}
			e := p.Get(id)
			stats.Inflight.Add(1)
			go func(id int, e *Entry) {
				defer stats.Inflight.Add(-1)
				p.runOne(ctx, cfg, stats, id, e)
			}(id, e)
		}

		if cfg.OnTick != nil && cfg.StatsEvery > 0 {
			total := stats.Sent.Load() + stats.Failed.Load()
			if total >= lastReportTotal+cfg.StatsEvery {
				lastReportTotal = total - (total % cfg.StatsEvery)
				cfg.OnTick(stats)
			}
		}
		time.Sleep(100 * time.Microsecond)
	}
}

func (p *WalletPool) runOne(parent context.Context, cfg LoopConfig, stats *pool.Stats, id int, e *Entry) {
	timeout := time.Duration(0)
	if cfg.SendTimeout != nil {
		timeout = cfg.SendTimeout()
	}
	if timeout <= 0 {
		timeout = 5 * time.Second
	}
	ctx, cancel := context.WithTimeout(parent, timeout)
	defer cancel()

	if cfg.Gate != nil && !cfg.Gate(ctx, e) {
		stats.Skipped.Add(1)
		return
	}

	seqno, fetched1, ok := p.acquireSeqno(ctx, cfg, stats, id, e, &stats.Seqno1)
	if !ok {
		return
	}
	if fetched1 {
		stats.Send1.Add(1)
	} else {
		stats.Send0.Add(1)
	}

	firstErr := cfg.Send(ctx, e, seqno)
	if firstErr == nil {
		stats.Sent.Add(1)
		return
	}

	e.mu.Lock()
	e.valid = false
	e.mu.Unlock()

	if cfg.NoRetry != nil && cfg.NoRetry() {
		p.logErr(cfg, id, e, seqno, "send", firstErr)
		stats.Failed.Add(1)
		return
	}

	stats.Retried.Add(1)
	seqno2, _, ok := p.acquireSeqno(ctx, cfg, stats, id, e, &stats.Seqno2)
	if !ok {
		p.logErr(cfg, id, e, seqno, "send", firstErr)
		return
	}
	stats.Send2.Add(1)
	if err2 := cfg.Send(ctx, e, seqno2); err2 != nil {
		p.logErr(cfg, id, e, seqno, "send", firstErr)
		p.logErr(cfg, id, e, seqno2, "retrySend", err2)
		e.mu.Lock()
		e.valid = false
		e.mu.Unlock()
		stats.Failed.Add(1)
		return
	}
	stats.Sent.Add(1)
}

func (p *WalletPool) acquireSeqno(ctx context.Context, cfg LoopConfig, stats *pool.Stats, id int, e *Entry, fetchCounter *atomic.Uint64) (uint32, bool, bool) {
	e.mu.Lock()
	needFetch := !e.valid
	var seqno uint32
	if !needFetch {
		seqno = e.seqno
		e.seqno++
	}
	e.mu.Unlock()
	if !needFetch {
		return seqno, false, true
	}

	fetchCounter.Add(1)
	fetched, err := cfg.FetchSeqno(ctx, e)
	if err != nil {
		p.logErr(cfg, id, e, 0, "fetchSeqno", err)
		stats.Failed.Add(1)
		return 0, true, false
	}
	e.mu.Lock()
	if !e.valid {
		e.seqno = fetched
		e.valid = true
	}
	seqno = e.seqno
	e.seqno++
	e.mu.Unlock()
	return seqno, true, true
}

func (p *WalletPool) logErr(cfg LoopConfig, id int, e *Entry, seqno uint32, phase string, err error) {
	if cfg.ErrLog != nil {
		cfg.ErrLog.Printf("w5=%d addr=%s seqno=%d phase=%s err=%+v", id, e.W5.Address().String(), seqno, phase, err)
	}
	if cfg.ErrSink != nil {
		cfg.ErrSink(phase, err.Error())
	}
}
