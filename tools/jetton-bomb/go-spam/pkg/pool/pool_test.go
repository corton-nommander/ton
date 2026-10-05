package pool

import (
	"context"
	"crypto/ed25519"
	"errors"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/xssnick/tonutils-go/address"

	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

func testPool(t *testing.T, count int) *Pool {
	t.Helper()
	pub, priv, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	minter := address.NewAddress(0, 0, make([]byte, 32))
	return New(count, pub, priv, minter, walletspam.LoadJettonWalletCode(), walletspam.LoadCode(walletspam.CodeHex))
}

// A false Gate must SKIP cleanly: Skipped++, no Send, no Sent/Failed, and crucially no seqno consumed
// (else the cache would desync a wallet we never actually sent to).
func TestRunOneGateSkips(t *testing.T) {
	p := testPool(t, 4)
	e := p.Get(1)
	stats := &Stats{}
	sendCalls := 0
	cfg := LoopConfig{
		SendTimeout: func() time.Duration { return time.Second },
		Gate:        func(context.Context, *Entry) bool { return false },
		Send:        func(context.Context, *Entry, uint32) error { sendCalls++; return nil },
		FetchSeqno:  func(context.Context, *Entry) (uint32, error) { return 0, nil },
	}
	p.runOne(context.Background(), cfg, stats, 1, e)

	if got := stats.Skipped.Load(); got != 1 {
		t.Fatalf("Skipped=%d want 1", got)
	}
	if got := stats.Sent.Load(); got != 0 {
		t.Fatalf("Sent=%d want 0", got)
	}
	if got := stats.Failed.Load(); got != 0 {
		t.Fatalf("Failed=%d want 0", got)
	}
	if sendCalls != 0 {
		t.Fatalf("Send called %d times, want 0", sendCalls)
	}
	e.mu.Lock()
	valid := e.valid
	e.mu.Unlock()
	if valid {
		t.Fatal("gate skip must not validate/consume the seqno cache")
	}
}

// A true Gate proceeds normally: Send runs, Sent++, Skipped stays 0.
func TestRunOneGateAllows(t *testing.T) {
	p := testPool(t, 4)
	e := p.Get(1)
	stats := &Stats{}
	cfg := LoopConfig{
		SendTimeout: func() time.Duration { return time.Second },
		Gate:        func(context.Context, *Entry) bool { return true },
		Send:        func(context.Context, *Entry, uint32) error { return nil },
		FetchSeqno:  func(context.Context, *Entry) (uint32, error) { return 5, nil },
	}
	p.runOne(context.Background(), cfg, stats, 1, e)

	if got := stats.Skipped.Load(); got != 0 {
		t.Fatalf("Skipped=%d want 0", got)
	}
	if got := stats.Sent.Load(); got != 1 {
		t.Fatalf("Sent=%d want 1", got)
	}
}

func TestRunPairTracksRemoteTargetSeqno(t *testing.T) {
	p := testPool(t, 4)
	relayer := p.Get(0)
	target := p.Get(1)
	stats := &Stats{}
	chainSeqno := map[uint32]uint32{0: 3, 1: 8}
	cfg := LoopConfig{
		SendTimeout:      func() time.Duration { return time.Second },
		RemotePendingFor: time.Second,
		FetchSeqno: func(_ context.Context, e *Entry) (uint32, error) {
			return chainSeqno[e.Wallet.SubwalletID], nil
		},
		SendPair: func(_ context.Context, _ *Entry, relayerSeqno uint32, _ *Entry, targetSeqno uint32) error {
			if relayerSeqno != 3 || targetSeqno != 8 {
				t.Fatalf("pair seqnos=(%d,%d), want (3,8)", relayerSeqno, targetSeqno)
			}
			return nil
		},
	}
	p.runPair(context.Background(), cfg, stats, 0, relayer, 1, target)

	if got := stats.Sent.Load(); got != 1 {
		t.Fatalf("Sent=%d want 1", got)
	}
	target.mu.Lock()
	pending, expected := target.remotePending, target.remoteSeqno
	target.mu.Unlock()
	if !pending || expected != 9 {
		t.Fatalf("target pending=%t expected=%d, want true/9", pending, expected)
	}
	relayer.mu.Lock()
	relayerPending, relayerExpected := relayer.remotePending, relayer.remoteSeqno
	relayer.mu.Unlock()
	if !relayerPending || relayerExpected != 4 {
		t.Fatalf("relayer pending=%t expected=%d, want true/4", relayerPending, relayerExpected)
	}

	// Once the internal request lands, the next use refetches 9 instead of trusting an optimistic cache.
	chainSeqno[1] = 9
	seqno, fetched, ok := p.acquireSeqno(
		context.Background(), cfg, stats, 1, target, &stats.Seqno1, 0,
	)
	if !ok || !fetched || seqno != 9 {
		t.Fatalf("next target seqno=(%d fetched=%t ok=%t), want (9,true,true)", seqno, fetched, ok)
	}
}

func TestRunPairFailureInvalidatesBothEntries(t *testing.T) {
	p := testPool(t, 4)
	relayer := p.Get(0)
	target := p.Get(1)
	stats := &Stats{}
	cfg := LoopConfig{
		SendTimeout: func() time.Duration { return time.Second },
		NoRetry:     func() bool { return true },
		FetchSeqno:  func(context.Context, *Entry) (uint32, error) { return 0, nil },
		SendPair: func(context.Context, *Entry, uint32, *Entry, uint32) error {
			return errors.New("send failed")
		},
	}
	p.runPair(context.Background(), cfg, stats, 0, relayer, 1, target)

	if got := stats.Failed.Load(); got != 1 {
		t.Fatalf("Failed=%d want 1", got)
	}
	for name, e := range map[string]*Entry{"relayer": relayer, "target": target} {
		e.mu.Lock()
		valid, pending := e.valid, e.remotePending
		e.mu.Unlock()
		if valid || pending {
			t.Fatalf("%s cache valid=%t pending=%t, want both false", name, valid, pending)
		}
	}
}

func TestPairEpochsDoNotOverlap(t *testing.T) {
	p := testPool(t, 2)
	stats := &Stats{}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	firstStarted := make(chan struct{})
	releaseFirst := make(chan struct{})
	secondStarted := make(chan struct{})
	var calls atomic.Uint32
	var chainSeqno atomic.Uint32
	cfg := LoopConfig{
		Rate:             1000,
		MaxInflight:      100,
		SendTimeout:      func() time.Duration { return time.Second },
		RemotePendingFor: time.Second,
		FetchSeqno: func(context.Context, *Entry) (uint32, error) {
			return chainSeqno.Load(), nil
		},
		SendPair: func(context.Context, *Entry, uint32, *Entry, uint32) error {
			switch calls.Add(1) {
			case 1:
				close(firstStarted)
				<-releaseFirst
				chainSeqno.Store(1)
			case 2:
				close(secondStarted)
				cancel()
			}
			return nil
		},
	}
	done := make(chan struct{})
	go func() {
		p.Run(ctx, cfg, stats)
		close(done)
	}()

	select {
	case <-firstStarted:
	case <-time.After(time.Second):
		t.Fatal("first pair did not start")
	}
	time.Sleep(20 * time.Millisecond)
	if got := calls.Load(); got != 1 {
		t.Fatalf("pair epochs overlapped: calls=%d before first completed", got)
	}
	close(releaseFirst)
	select {
	case <-secondStarted:
	case <-time.After(time.Second):
		t.Fatal("second pair did not start after epoch barrier")
	}
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("pair loop did not stop")
	}
}

// A windowed run must touch ONLY its sub-pool, and cover it exactly once per epoch. This is what makes
// the bot's "slice the 2.5M pool into fixed sub-pools and pick one" scheme spend the pool evenly: a run
// confined to [start, start+n) can neither leak into a neighbouring sub-pool nor revisit a wallet early.
func TestRunWindowStaysInRange(t *testing.T) {
	p := testPool(t, 40)
	const start, count = 12, 8

	var mu sync.Mutex
	hits := map[uint32]int{}
	ctx, cancel := context.WithCancel(context.Background())
	stats := &Stats{}

	cfg := LoopConfig{
		Rate:        2000,
		MaxInflight: 64,
		WindowStart: start,
		WindowCount: count,
		Send: func(_ context.Context, e *Entry, _ uint32) error {
			mu.Lock()
			hits[e.Wallet.SubwalletID]++
			total := 0
			for _, n := range hits {
				total += n
			}
			mu.Unlock()
			if total >= count { // one full epoch
				cancel()
			}
			return nil
		},
		FetchSeqno: func(context.Context, *Entry) (uint32, error) { return 0, nil },
	}
	p.Run(ctx, cfg, stats)

	mu.Lock()
	defer mu.Unlock()
	if len(hits) == 0 {
		t.Fatal("no sends happened")
	}
	for id, n := range hits {
		if id < start || id >= start+count {
			t.Fatalf("subwallet %d is outside the window [%d,%d)", id, start, start+count)
		}
		if n > 2 { // an epoch boundary can start a second pass before cancel lands
			t.Fatalf("subwallet %d hit %d times in ~one epoch — window is not shuffle-once", id, n)
		}
	}
}

// An out-of-range or zero window must fall back to the whole pool rather than silently sending nothing.
func TestRunWindowFallsBackToWholePool(t *testing.T) {
	for _, tc := range []struct {
		name         string
		start, count int
	}{
		{"zero count", 0, 0},
		{"negative start", -5, 4},
		{"past the end", 8, 99},
	} {
		t.Run(tc.name, func(t *testing.T) {
			p := testPool(t, 10)
			var seen atomic.Int64
			ctx, cancel := context.WithCancel(context.Background())
			cfg := LoopConfig{
				Rate: 2000, MaxInflight: 32,
				WindowStart: tc.start, WindowCount: tc.count,
				Send: func(_ context.Context, e *Entry, _ uint32) error {
					if e.Wallet.SubwalletID >= 10 {
						t.Errorf("subwallet %d outside the pool", e.Wallet.SubwalletID)
					}
					if seen.Add(1) >= 10 {
						cancel()
					}
					return nil
				},
				FetchSeqno: func(context.Context, *Entry) (uint32, error) { return 0, nil },
			}
			p.Run(ctx, cfg, &Stats{})
			if seen.Load() == 0 {
				t.Fatal("fallback window sent nothing")
			}
		})
	}
}
