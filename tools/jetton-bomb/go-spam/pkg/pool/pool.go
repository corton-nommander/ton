// Package pool provides the shared 100k-WalletSpam pool, seqno cache, and token-bucket
// driver used by cmd/spam and all WalletSpam-backed bot commands.
package pool

import (
	"context"
	"crypto/ed25519"
	"errors"
	"fmt"
	"log"
	"math/rand"
	"sync"
	"sync/atomic"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

// Entry is one WalletSpam in the pool with its mutex-protected seqno cache.
type Entry struct {
	Wallet        *walletspam.WalletSpam
	mu            sync.Mutex
	seqno         uint32
	valid         bool
	remotePending bool
	remoteSeqno   uint32
	remoteUntil   time.Time
}

// SendFunc is the per-id work the loop calls. Implementations:
//   - external-only: BuildExternalBody and post via Backend.SendExternalMessage.
//   - jetton: build inner JettonWallet.transferMessage + WalletSpam wrapping then post.
//
// fetchSeqno is provided to consult the chain when the cache is invalid.
type SendFunc func(ctx context.Context, e *Entry, seqno uint32) error

// PairSendFunc submits one relayer external that asynchronously consumes targetSeqno through a
// signed internal request. Both entries come from this Pool and therefore share the same seqno cache.
type PairSendFunc func(
	ctx context.Context,
	relayer *Entry,
	relayerSeqno uint32,
	target *Entry,
	targetSeqno uint32,
) error

// FetchSeqno reads the current chain seqno of an Entry's wallet.
type FetchSeqno func(ctx context.Context, e *Entry) (uint32, error)

// Pool owns the 100k WalletSpam pool keyed by subwallet id.
type Pool struct {
	count  int
	pub    ed25519.PublicKey
	priv   ed25519.PrivateKey
	minter *address.Address
	jwCode *cell.Cell
	code   *cell.Cell

	mu      sync.RWMutex
	entries map[int]*Entry
}

// New creates a Pool bound to the given keypair, minter address, jetton-wallet code,
// WalletSpam code, and pool size. The minter + jetton-wallet code are part of each
// WalletSpam's storage, so they determine the pool address. Entries are created lazily.
func New(count int, pub ed25519.PublicKey, priv ed25519.PrivateKey, minter *address.Address, jwCode, code *cell.Cell) *Pool {
	return &Pool{
		count:   count,
		pub:     pub,
		priv:    priv,
		minter:  minter,
		jwCode:  jwCode,
		code:    code,
		entries: make(map[int]*Entry, count),
	}
}

// Count returns the size of the address space.
func (p *Pool) Count() int { return p.count }

// Get returns the Entry for the given subwallet id, creating it lazily.
func (p *Pool) Get(id int) *Entry {
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
	e = &Entry{Wallet: walletspam.New(uint32(id), p.pub, p.priv, p.minter, p.jwCode, p.code)}
	p.entries[id] = e
	return e
}

// Stats are atomic counters watched by the loop.
//
// Retried counts events where the first Send returned an error and we
// refetched + resent before counting Sent/Failed. Useful as a "how stale was
// our cache" signal: at high freq with hash-order block production, the
// first send fails LS-simulation routinely and the retry saves it.
//
// Send0/Seqno1/Send1/Seqno2/Send2 break down the backend-RPC pipeline so the
// caller can compute the actual RPS we throw at the LS/HTTP API:
//   - Send0  = first sends with WARM cache (no preceding fetch)
//   - Seqno1 = first-attempt seqno fetches (one direct; two or more for paired/pending sends)
//   - Send1  = first sends preceded by one or more Seqno1 fetches
//   - Seqno2 = retry-path seqno fetches (one direct; two or more for paired/pending sends)
//   - Send2  = retry sends after a successful Seqno2 fetch
//
// Total backend RPCs/s = Send0 + Seqno1 + Send1 + Seqno2 + Send2 + BatchSend + BatchRetry.
type Stats struct {
	Sent      atomic.Uint64
	Failed    atomic.Uint64
	Throttled atomic.Uint64
	// Skipped counts events dropped by the pre-send Gate hook BEFORE any seqno is consumed or send
	// attempted (so they never touch Sent/Failed and can't desync the seqno cache). The bot's
	// /check_balance uses it to skip drained pool wallets. 0 when no Gate is set.
	Skipped  atomic.Uint64
	Retried  atomic.Uint64
	Send0    atomic.Uint64
	Seqno1   atomic.Uint64
	Send1    atomic.Uint64
	Seqno2   atomic.Uint64
	Send2    atomic.Uint64
	Inflight atomic.Int64
	// BatchSend/BatchRetry count actual highload RPC attempts. Batches counts accepted batch externals;
	// Sent/Failed remain per logical gasless request, not per external or confirmed on-chain transfer.
	BatchSend  atomic.Uint64
	BatchRetry atomic.Uint64
	Batches    atomic.Uint64
}

// CacheStats is a point-in-time summary of pool warmness.
type CacheStats struct {
	Total       int // address space size (== p.count)
	Touched     int // entries ever picked (lazy-created)
	SeqnoCached int // entries with valid seqno cache
}

// CacheStats walks the entry map under read-lock and aggregates warmness.
// Called rarely (status messages); fine to be O(touched).
func (p *Pool) CacheStats() CacheStats {
	p.mu.RLock()
	defer p.mu.RUnlock()
	cs := CacheStats{Total: p.count, Touched: len(p.entries)}
	for _, e := range p.entries {
		e.mu.Lock()
		if e.valid {
			cs.SeqnoCached++
		}
		e.mu.Unlock()
	}
	return cs
}

// LoopConfig drives the token-bucket spam loop.
type LoopConfig struct {
	Rate        float64                           // sends per second
	MaxInflight int64                             // cap on concurrent in-flight goroutines
	SendTimeout func() time.Duration              // ctx timeout for a single send; called per request so it can be tuned at runtime
	ErrLog      *log.Logger                       // raw per-error log target; usually nil when ErrSink is set
	ErrSink     func(phase string, errMsg string) // optional: also receives each error for aggregation (bot uses this)
	OnTick      func(s *Stats)                    // called inside the inner loop after stats updates (optional)
	StatsEvery  uint64                            // emit OnTick every N total events; 0 = no callback
	// NoRetry, when non-nil and returning true, disables the refetch+resend on a failed first send:
	// the send is single-attempt (the pre-"Retries" behavior of commit d09c5d4). It's a func (like
	// SendTimeout) so it can be toggled live (e.g. the bot's /retry). nil => retries on (default).
	NoRetry func() bool
	// NoSeqnoCache, when true, disables the per-entry warm seqno cache: every send fetches the wallet's
	// seqno from chain via FetchSeqno instead of trusting the cached last+1. Used by /hamster_kombat over
	// the 2.5M pool — the epoch is so long that warm hits still return a correct value, but the operator
	// prefers a fresh per-send read there. Costs one extra seqno RPC per send (backend RPS ~doubles), so
	// leave it false (default) for the 100k /jetton pool and cmd/spam.
	NoSeqnoCache bool
	// Gate, when non-nil, is a pre-send predicate consulted at the very top of runOne (before any seqno
	// is acquired or send attempted). Returning false SKIPS this event: stats.Skipped++ and return, with
	// NO seqno consumed (so the cache can't desync) and NO Sent/Failed counted. Returning true proceeds
	// normally. It runs under the per-send timeout ctx and may do an RPC. nil => never skip (default).
	// Used by the bot's /check_balance to drop drained wallets. cmd/spam leaves it nil.
	Gate func(ctx context.Context, e *Entry) bool
	// Prepare runs after Gate but before the first seqno RPC. The streaming sampler
	// uses it to choose and subscribe to one real recipient per second.
	Prepare func(ctx context.Context, e *Entry) context.Context
	Finish  func(ctx context.Context)
	// WindowStart / WindowCount restrict the driver to the contiguous subwallet range
	// [WindowStart, WindowStart+WindowCount) instead of the whole pool. The shuffle-once epoch then covers
	// exactly that window, so a run touches one sub-pool and nothing else.
	//
	// This is how the bot spends the 2.5M pool evenly: it slices the pool into fixed sub-pools of the
	// configured size and picks one per run, rather than taking a random offset (which would revisit the
	// middle far more often than the two ends). WindowCount <= 0 means the whole pool.
	WindowStart int
	WindowCount int
	Send        SendFunc
	// BatchedSend delegates retries and RPC accounting to the shared batcher. No per-item resend is
	// allowed: a retry must retain the immutable external's query_id and go through its address limiter.
	BatchedSend bool
	// SendPair switches the scheduler to disjoint shuffled relayer/target pairs. Both caches are held
	// in remote-pending state after outer submission until their on-chain seqnos catch up or this
	// timeout expires. Send and SendPair are mutually exclusive.
	SendPair PairSendFunc
	// Also used with BatchedSend to prevent reusing a target before its submitted sint executes.
	RemotePendingFor time.Duration
	FetchSeqno       FetchSeqno
}

// Run drives the spam loop until ctx is cancelled. Returns when ctx is done.
// Caller is responsible for funding/deploying wallets and for choosing the size of the
// random id space (= p.Count()).
func (p *Pool) Run(ctx context.Context, cfg LoopConfig, stats *Stats) {
	if cfg.Rate <= 0 || (cfg.SendPair != nil && p.count < 2) {
		return
	}
	maxOwed := cfg.Rate
	if maxOwed < 1 {
		maxOwed = 1
	}
	last := time.Now()
	owed := 0.0
	var lastReportTotal uint64

	// Shuffle-once iteration: every wallet gets hit exactly once per epoch,
	// then we reshuffle for a new random order. Avoids birthday-paradox
	// collisions vs. picking ids uniformly each tick.
	//
	// The permutation covers the WINDOW, and ids are offset by windowStart — so a windowed run walks
	// exactly its sub-pool, with the same once-per-epoch property the full-pool run has.
	windowStart, windowCount := cfg.WindowStart, cfg.WindowCount
	if windowCount <= 0 || windowStart < 0 || windowStart+windowCount > p.count {
		windowStart, windowCount = 0, p.count
	}
	if cfg.SendPair != nil && windowCount < 2 {
		return
	}
	perm := rand.Perm(windowCount)
	cursor := 0
	var workers sync.WaitGroup

	for {
		select {
		case <-ctx.Done():
			workers.Wait()
			return
		default:
		}
		now := time.Now()
		dt := now.Sub(last).Seconds()
		last = now
		owed += dt * cfg.Rate
		if owed > maxOwed {
			owed = maxOwed
		}
		for owed >= 1 {
			owed -= 1
			if stats.Inflight.Load() >= cfg.MaxInflight {
				stats.Throttled.Add(1)
				continue
			}
			if cfg.SendPair != nil {
				// Consume two adjacent shuffled ids. With an odd pool, leave the final id unused for this
				// epoch rather than pairing any wallet twice. Do not overlap pair epochs: otherwise a
				// reshuffle could schedule A->B and B->A concurrently and cyclically wait on reservations.
				if cursor+1 >= len(perm) {
					workers.Wait()
					if ctx.Err() != nil {
						return
					}
					rand.Shuffle(len(perm), func(i, j int) { perm[i], perm[j] = perm[j], perm[i] })
					cursor = 0
					last = time.Now()
					owed = 0
				}
				relayerID, targetID := windowStart+perm[cursor], windowStart+perm[cursor+1]
				cursor += 2
				relayer, target := p.Get(relayerID), p.Get(targetID)
				stats.Inflight.Add(1)
				workers.Add(1)
				go func() {
					defer workers.Done()
					defer stats.Inflight.Add(-1)
					p.runPair(ctx, cfg, stats, relayerID, relayer, targetID, target)
				}()
			} else {
				id := windowStart + perm[cursor]
				cursor++
				if cursor >= len(perm) {
					rand.Shuffle(len(perm), func(i, j int) { perm[i], perm[j] = perm[j], perm[i] })
					cursor = 0
				}
				e := p.Get(id)
				stats.Inflight.Add(1)
				workers.Add(1)
				go func(id int, e *Entry) {
					defer workers.Done()
					defer stats.Inflight.Add(-1)
					p.runOne(ctx, cfg, stats, id, e)
				}(id, e)
			}
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

func (p *Pool) runOne(parent context.Context, cfg LoopConfig, stats *Stats, id int, e *Entry) {
	timeout := time.Duration(0)
	if cfg.SendTimeout != nil {
		timeout = cfg.SendTimeout()
	}
	if timeout <= 0 {
		timeout = 5 * time.Second
	}
	ctx, cancel := context.WithTimeout(parent, timeout)
	defer cancel()

	// Pre-send gate (e.g. /check_balance): skip BEFORE consuming a seqno so a drained wallet neither
	// desyncs its cache nor counts as Sent/Failed — it's a clean Skipped, like Throttled.
	if cfg.Gate != nil && !cfg.Gate(ctx, e) {
		stats.Skipped.Add(1)
		return
	}
	if cfg.Prepare != nil {
		ctx = cfg.Prepare(ctx, e)
		if cfg.Finish != nil {
			defer cfg.Finish(ctx)
		}
	}

	seqno, fetched1, ok := p.acquireSeqno(ctx, cfg, stats, id, e, &stats.Seqno1, cfg.RemotePendingFor)
	if !ok {
		return
	}
	if !cfg.BatchedSend && fetched1 {
		stats.Send1.Add(1)
	} else if !cfg.BatchedSend {
		stats.Send0.Add(1)
	}

	firstErr := cfg.Send(ctx, e, seqno)
	if firstErr == nil {
		if cfg.RemotePendingFor > 0 {
			refreshRemotePending(e, cfg.RemotePendingFor)
		}
		stats.Sent.Add(1)
		return
	}

	// Invalidate a failed direct send or a batch item dropped before dispatch. An uncertain batch
	// submission keeps its remote reservation until the chain advances or the request expires.
	var uncertain interface{ MayHaveBeenSent() bool }
	if cfg.BatchedSend && errors.As(firstErr, &uncertain) && uncertain.MayHaveBeenSent() {
		// Timeout/error after batch dispatch is not evidence that the target's nonce is unused.
		refreshRemotePending(e, cfg.RemotePendingFor)
	} else {
		invalidateEntry(e)
	}

	// NoRetry (opt-in): single-attempt mode, the pre-"Retries" behavior of commit d09c5d4. Count the
	// failure and stop — no refetch+resend. Retried stays 0 in this mode.
	if cfg.BatchedSend || cfg.NoRetry != nil && cfg.NoRetry() {
		if cfg.ErrLog != nil {
			cfg.ErrLog.Printf("subwallet=%d addr=%s seqno=%d phase=send err=%+v",
				id, e.Wallet.Address().String(), seqno, firstErr)
		}
		if cfg.ErrSink != nil {
			cfg.ErrSink("send", firstErr.Error())
		}
		stats.Failed.Add(1)
		return
	}

	// The dominant failure at high freq is a stale cached seqno: LS does TVM-simulation before
	// forwarding, and rejects when its view of stored_seqno hasn't caught up with what our cache
	// assumed. Refetch from the same LS — its current view IS the truth for the retry — and try once
	// more before counting Failed.
	stats.Retried.Add(1)

	seqno2, _, ok := p.acquireSeqno(ctx, cfg, stats, id, e, &stats.Seqno2, 0)
	if !ok {
		if cfg.ErrLog != nil {
			cfg.ErrLog.Printf("subwallet=%d addr=%s seqno=%d phase=send err=%+v",
				id, e.Wallet.Address().String(), seqno, firstErr)
		}
		if cfg.ErrSink != nil {
			cfg.ErrSink("send", firstErr.Error())
		}
		return
	}
	stats.Send2.Add(1)

	if err2 := cfg.Send(ctx, e, seqno2); err2 != nil {
		if cfg.ErrLog != nil {
			cfg.ErrLog.Printf("subwallet=%d addr=%s seqno=%d phase=send err=%+v",
				id, e.Wallet.Address().String(), seqno, firstErr)
			cfg.ErrLog.Printf("subwallet=%d addr=%s seqno=%d phase=retrySend err=%+v",
				id, e.Wallet.Address().String(), seqno2, err2)
		}
		if cfg.ErrSink != nil {
			cfg.ErrSink("send", firstErr.Error())
			cfg.ErrSink("retrySend", err2.Error())
		}
		invalidateEntry(e)
		stats.Failed.Add(1)
		return
	}
	stats.Sent.Add(1)
}

func (p *Pool) runPair(
	parent context.Context,
	cfg LoopConfig,
	stats *Stats,
	relayerID int,
	relayer *Entry,
	targetID int,
	target *Entry,
) {
	timeout := time.Duration(0)
	if cfg.SendTimeout != nil {
		timeout = cfg.SendTimeout()
	}
	if timeout <= 0 {
		timeout = 5 * time.Second
	}
	ctx, cancel := context.WithTimeout(parent, timeout)
	defer cancel()

	if cfg.Gate != nil && !cfg.Gate(ctx, relayer) {
		stats.Skipped.Add(1)
		return
	}

	pendingFor := cfg.RemotePendingFor
	if pendingFor <= 0 {
		pendingFor = 2 * time.Minute
	}
	relayerSeqno, relayerFetched, ok := p.acquireSeqno(
		ctx, cfg, stats, relayerID, relayer, &stats.Seqno1, pendingFor,
	)
	if !ok {
		return
	}
	targetSeqno, targetFetched, ok := p.acquireSeqno(
		ctx, cfg, stats, targetID, target, &stats.Seqno1, pendingFor,
	)
	if !ok {
		invalidateEntry(relayer)
		return
	}
	if relayerFetched || targetFetched {
		stats.Send1.Add(1)
	} else {
		stats.Send0.Add(1)
	}
	refreshRemotePending(relayer, pendingFor)
	refreshRemotePending(target, pendingFor)

	firstErr := cfg.SendPair(ctx, relayer, relayerSeqno, target, targetSeqno)
	if firstErr == nil {
		refreshRemotePending(relayer, pendingFor)
		refreshRemotePending(target, pendingFor)
		stats.Sent.Add(1)
		return
	}
	invalidateEntry(relayer)
	invalidateEntry(target)

	if cfg.NoRetry != nil && cfg.NoRetry() {
		logPairError(cfg, "send", relayerID, relayer, relayerSeqno, targetID, target, targetSeqno, firstErr)
		stats.Failed.Add(1)
		return
	}
	stats.Retried.Add(1)

	relayerSeqno2, _, ok := p.acquireSeqno(
		ctx, cfg, stats, relayerID, relayer, &stats.Seqno2, pendingFor,
	)
	if !ok {
		logPairError(cfg, "send", relayerID, relayer, relayerSeqno, targetID, target, targetSeqno, firstErr)
		return
	}
	targetSeqno2, _, ok := p.acquireSeqno(
		ctx, cfg, stats, targetID, target, &stats.Seqno2, pendingFor,
	)
	if !ok {
		invalidateEntry(relayer)
		logPairError(cfg, "send", relayerID, relayer, relayerSeqno, targetID, target, targetSeqno, firstErr)
		return
	}
	stats.Send2.Add(1)
	refreshRemotePending(relayer, pendingFor)
	refreshRemotePending(target, pendingFor)

	if err2 := cfg.SendPair(ctx, relayer, relayerSeqno2, target, targetSeqno2); err2 != nil {
		logPairError(cfg, "send", relayerID, relayer, relayerSeqno, targetID, target, targetSeqno, firstErr)
		logPairError(cfg, "retrySend", relayerID, relayer, relayerSeqno2, targetID, target, targetSeqno2, err2)
		invalidateEntry(relayer)
		invalidateEntry(target)
		stats.Failed.Add(1)
		return
	}
	refreshRemotePending(relayer, pendingFor)
	refreshRemotePending(target, pendingFor)
	stats.Sent.Add(1)
}

func logPairError(
	cfg LoopConfig,
	phase string,
	relayerID int,
	relayer *Entry,
	relayerSeqno uint32,
	targetID int,
	target *Entry,
	targetSeqno uint32,
	err error,
) {
	if cfg.ErrLog != nil {
		cfg.ErrLog.Printf(
			"relayer=%d addr=%s seqno=%d target=%d addr=%s seqno=%d phase=%s err=%+v",
			relayerID,
			relayer.Wallet.Address().String(),
			relayerSeqno,
			targetID,
			target.Wallet.Address().String(),
			targetSeqno,
			phase,
			err,
		)
	}
	if cfg.ErrSink != nil {
		cfg.ErrSink(phase, err.Error())
	}
}

func invalidateEntry(e *Entry) {
	e.mu.Lock()
	e.valid = false
	e.remotePending = false
	e.mu.Unlock()
}

func refreshRemotePending(e *Entry, pendingFor time.Duration) {
	e.mu.Lock()
	if e.remotePending {
		e.remoteUntil = time.Now().Add(pendingFor)
	}
	e.mu.Unlock()
}

// acquireSeqno returns the next seqno to use for e, lazily fetching from chain
// when the cache is invalid. On fetch failure it counts Failed and surfaces the
// error via ErrLog/ErrSink, returning ok=false so the caller can bail. The
// fetchCounter atomic is bumped exactly when an RPC round-trip is attempted,
// so callers can distinguish warm-cache hits (no RPC) from cold-cache fetches.
func (p *Pool) acquireSeqno(
	ctx context.Context,
	cfg LoopConfig,
	stats *Stats,
	id int,
	e *Entry,
	fetchCounter *atomic.Uint64,
	remotePendingFor time.Duration,
) (uint32, bool, bool) {
	e.mu.Lock()
	needFetch := cfg.NoSeqnoCache || !e.valid || e.remotePending
	var seqno uint32
	if !needFetch {
		seqno = e.seqno
		e.seqno++
		if remotePendingFor > 0 {
			e.remotePending = true
			e.remoteSeqno = e.seqno
			e.remoteUntil = time.Now().Add(remotePendingFor)
			e.valid = false
		}
	}
	e.mu.Unlock()

	if !needFetch {
		return seqno, false, true
	}

	for {
		fetchCounter.Add(1)
		fetched, err := cfg.FetchSeqno(ctx, e)
		if err != nil {
			reportFetchError(cfg, stats, id, e, err)
			return 0, true, false
		}

		e.mu.Lock()
		if e.remotePending && fetched < e.remoteSeqno && time.Now().Before(e.remoteUntil) {
			e.mu.Unlock()
			select {
			case <-ctx.Done():
				reportFetchError(cfg, stats, id, e, ctx.Err())
				return 0, true, false
			case <-time.After(100 * time.Millisecond):
				continue
			}
		}
		if e.remotePending {
			e.remotePending = false
		}
		if !e.valid {
			e.seqno = fetched
			e.valid = true
		}
		seqno = e.seqno
		e.seqno++
		if remotePendingFor > 0 {
			e.remotePending = true
			e.remoteSeqno = e.seqno
			e.remoteUntil = time.Now().Add(remotePendingFor)
			e.valid = false
		}
		e.mu.Unlock()
		return seqno, true, true
	}
}

func reportFetchError(cfg LoopConfig, stats *Stats, id int, e *Entry, err error) {
	if cfg.ErrLog != nil {
		cfg.ErrLog.Printf("subwallet=%d addr=%s phase=fetchSeqno err=%+v",
			id, e.Wallet.Address().String(), err)
	}
	if cfg.ErrSink != nil {
		cfg.ErrSink("fetchSeqno", err.Error())
	}
	stats.Failed.Add(1)
}

// FormatStats is a small helper for human-readable progress lines.
func FormatStats(s *Stats, sinceSent, sinceFailed, sinceThrottled uint64, sinceTime time.Time) string {
	now := time.Now()
	intervalSec := now.Sub(sinceTime).Seconds()
	if intervalSec <= 0 {
		intervalSec = 1
	}
	sentNow := s.Sent.Load()
	failedNow := s.Failed.Load()
	throttledNow := s.Throttled.Load()
	sRate := float64(sentNow-sinceSent) / intervalSec
	fRate := float64(failedNow-sinceFailed) / intervalSec
	tRate := float64(throttledNow-sinceThrottled) / intervalSec
	skipped := ""
	if n := s.Skipped.Load(); n > 0 { // only shown when a Gate (check-balance) is active
		skipped = fmt.Sprintf("skipped: %d, ", n)
	}
	text := fmt.Sprintf("sent: %d (%.0f/s), failed: %d (%.0f/s), throttled: %d (%.0f/s), %sinflight: %d",
		sentNow, sRate, failedNow, fRate, throttledNow, tRate, skipped, s.Inflight.Load())
	if s.BatchSend.Load() > 0 {
		text += fmt.Sprintf(", batches: %d, batch RPCs: %d (+%d retries)", s.Batches.Load(), s.BatchSend.Load(), s.BatchRetry.Load())
	}
	return text
}
