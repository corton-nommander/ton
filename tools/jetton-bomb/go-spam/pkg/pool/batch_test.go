package pool

import (
	"context"
	"errors"
	"sync/atomic"
	"testing"
	"time"
)

func TestBatchedSendReservesNonceUntilChainAdvances(t *testing.T) {
	p := testPool(t, 2)
	e := p.Get(0)
	s := &Stats{}
	var chain, sends atomic.Uint32
	cfg := LoopConfig{
		BatchedSend: true, RemotePendingFor: time.Minute, SendTimeout: func() time.Duration { return 30 * time.Millisecond },
		FetchSeqno: func(context.Context, *Entry) (uint32, error) { return chain.Load(), nil },
		Send: func(_ context.Context, _ *Entry, seqno uint32) error {
			if seqno != sends.Load() {
				t.Errorf("seqno=%d, sends=%d", seqno, sends.Load())
			}
			sends.Add(1)
			s.BatchSend.Add(1)
			s.Batches.Add(1)
			return nil
		},
	}
	p.runOne(context.Background(), cfg, s, 0, e)
	p.runOne(context.Background(), cfg, s, 0, e) // chain still at 0: do not submit another sint
	if sends.Load() != 1 {
		t.Fatal("reused a pending target nonce")
	}
	chain.Store(1)
	p.runOne(context.Background(), cfg, s, 0, e)
	if sends.Load() != 2 || s.Sent.Load() != 2 {
		t.Fatal("did not resume after on-chain seqno advanced")
	}
	if s.Send0.Load()+s.Send1.Load()+s.Send2.Load() != 0 {
		t.Fatal("counted logical sends as external RPCs")
	}
	if s.BatchSend.Load() != 2 {
		t.Fatal("incorrect batch RPC count")
	}
}

type uncertainBatchError struct{}

func (uncertainBatchError) Error() string         { return "batch submission timed out" }
func (uncertainBatchError) MayHaveBeenSent() bool { return true }

func TestBatchedSendDoesNotRetryItemsOrReleaseUncertainNonce(t *testing.T) {
	p := testPool(t, 2)
	e := p.Get(0)
	s := &Stats{}
	var sends int
	cfg := LoopConfig{BatchedSend: true, RemotePendingFor: time.Minute,
		FetchSeqno: func(context.Context, *Entry) (uint32, error) { return 0, nil },
		Send:       func(context.Context, *Entry, uint32) error { sends++; return uncertainBatchError{} },
	}
	p.runOne(context.Background(), cfg, s, 0, e)
	if sends != 1 || s.Retried.Load() != 0 {
		t.Fatal("per-item retry bypassed batcher")
	}
	if !e.remotePending || e.valid {
		t.Fatal("released nonce after uncertain submission")
	}
	cfg.Send = func(context.Context, *Entry, uint32) error { return errors.New("cancelled before batch dispatch") }
	e.remotePending = false
	p.runOne(context.Background(), cfg, s, 0, e)
	if e.remotePending || e.valid {
		t.Fatal("kept reservation for a request never dispatched")
	}
}
