package main

import (
	"context"
	"errors"
	"io"
	"math/big"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
)

// fakeBackend implements backend.Backend; only RunGetMethod matters for the readiness wait. Per-address:
// an address becomes "deployed" (non-empty stack, nil error) on its readyAfter-th probe; before that it
// errors like an uninitialized account. emptyReady makes it return (nil, empty-stack) even once "ready",
// to exercise the len(stack)>0 guard.
type fakeBackend struct {
	mu         sync.Mutex
	calls      map[string]int
	readyAfter map[string]int // 0 => never ready (always errors)
	emptyReady map[string]bool
	seqno      uint32
	seqnoErr   error
}

func newFake() *fakeBackend {
	return &fakeBackend{
		calls:      map[string]int{},
		readyAfter: map[string]int{},
		emptyReady: map[string]bool{},
		seqnoErr:   errors.New("uninitialized account"),
	}
}

func (f *fakeBackend) RunGetMethod(_ context.Context, addr *address.Address, _ string, _ ...any) ([]backend.StackValue, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	k := addr.String()
	f.calls[k]++
	ra := f.readyAfter[k]
	if ra > 0 && f.calls[k] >= ra {
		if f.emptyReady[k] {
			return []backend.StackValue{}, nil // pathological: nil err but nothing on the stack
		}
		return []backend.StackValue{{Kind: "int", Int: big.NewInt(1)}}, nil
	}
	return nil, errors.New("uninitialized account")
}

func (f *fakeBackend) Kind() backend.Kind { return backend.KindHTTP }
func (f *fakeBackend) GetSeqno(context.Context, *address.Address) (uint32, error) {
	return f.seqno, f.seqnoErr
}
func (f *fakeBackend) GetBalance(context.Context, *address.Address) (*big.Int, error) {
	return big.NewInt(0), errors.New("unused")
}
func (f *fakeBackend) SendExternalMessage(context.Context, *address.Address, *cell.Cell) error {
	return nil
}
func (f *fakeBackend) SendExternalMessageWithInit(context.Context, *address.Address, *cell.Cell, *tlb.StateInit) error {
	return nil
}
func (f *fakeBackend) Description() string { return "fake" }

// testAddrs builds n distinct workchain-0 addresses; the last stands in for the terminal wallet.
func testAddrs(n int) []*address.Address {
	out := make([]*address.Address, n)
	for i := 0; i < n; i++ {
		data := make([]byte, 32)
		data[0] = byte(i)
		data[31] = byte(i >> 8)
		out[i] = address.NewAddress(0, 0, data)
	}
	return out
}

func idsFor(addrs []*address.Address) []uint32 {
	ids := make([]uint32, len(addrs))
	for i := range addrs {
		ids[i] = uint32(i)
	}
	return ids
}

func TestWaitReadyCompletesOnTerminal(t *testing.T) {
	// The terminal (last sample) deploying is the completion signal — the whole pool is up. Here the first
	// nine are up immediately and the terminal comes up on its 3rd probe; the wait must complete.
	addrs := testAddrs(10)
	ids := idsFor(addrs)
	f := newFake()
	for _, a := range addrs[:9] {
		f.readyAfter[a.String()] = 1
	}
	f.readyAfter[addrs[9].String()] = 3 // terminal deploys on the 3rd probe

	if err := waitReady(io.Discard, f, ids, addrs, "get_wallet_data", time.Millisecond, 5*time.Second); err != nil {
		t.Fatalf("expected completion when the terminal deploys, got: %v", err)
	}
}

func TestWaitReadyTimeoutTerminalMissing(t *testing.T) {
	// Nine of ten samples up but the terminal never deploys (the stuck-tail case) => must time out, NOT
	// falsely report success — a half-built pool would give the spammer a permanent failed%.
	addrs := testAddrs(10)
	ids := idsFor(addrs)
	f := newFake()
	for _, a := range addrs[:9] {
		f.readyAfter[a.String()] = 1
	}
	// addrs[9] (terminal) stays at readyAfter 0 => never ready.
	start := time.Now()
	err := waitReady(io.Discard, f, ids, addrs, "seqno", time.Millisecond, 40*time.Millisecond)
	if err == nil {
		t.Fatal("expected timeout error, got nil")
	}
	if !strings.Contains(err.Error(), "not deployed") {
		t.Fatalf("expected 'not deployed' in error, got: %v", err)
	}
	if elapsed := time.Since(start); elapsed > 2*time.Second {
		t.Fatalf("timeout overshot badly: %s", elapsed)
	}
}

func TestWaitReadyEmptyStackTerminalNotReady(t *testing.T) {
	// The terminal answering (nil error, empty stack) must NOT count as deployed => the wait times out.
	addrs := testAddrs(10)
	ids := idsFor(addrs)
	f := newFake()
	for _, a := range addrs {
		f.readyAfter[a.String()] = 1
	}
	f.emptyReady[addrs[9].String()] = true // terminal yields an empty stack

	if err := waitReady(io.Discard, f, ids, addrs, "get_wallet_data", time.Millisecond, 40*time.Millisecond); err == nil {
		t.Fatal("terminal empty result stack must not count as deployed; expected timeout error")
	}
}

func TestInspectPoolReady(t *testing.T) {
	addrs := testAddrs(2)
	f := newFake()
	f.readyAfter[addrs[1].String()] = 1

	state, _ := inspectPool(f, addrs[0], addrs[1], "get_wallet_data")
	if state != poolReady {
		t.Fatalf("expected poolReady, got %d", state)
	}
}

func TestInspectPoolDeploying(t *testing.T) {
	addrs := testAddrs(2)
	f := newFake()
	f.seqno = 42
	f.seqnoErr = nil

	state, seq := inspectPool(f, addrs[0], addrs[1], "get_wallet_data")
	if state != poolDeploying || seq != 42 {
		t.Fatalf("expected poolDeploying seqno 42, got state=%d seqno=%d", state, seq)
	}
}

func TestInspectPoolMissing(t *testing.T) {
	addrs := testAddrs(2)
	f := newFake()

	state, _ := inspectPool(f, addrs[0], addrs[1], "get_wallet_data")
	if state != poolMissing {
		t.Fatalf("expected poolMissing, got %d", state)
	}
}
