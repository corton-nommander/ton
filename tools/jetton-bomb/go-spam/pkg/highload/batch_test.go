package highload

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/gram"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

func batchFixture(t *testing.T, n, shard int) ([NumShards]*HighloadWallet, []*walletspam.WalletSpam, *cell.Cell) {
	t.Helper()
	pub, priv := testKey(t, 0x67)
	set := NewShardSet(pub, priv, DefaultTimeout)
	minter := address.MustParseAddr("kQCy3cWuUXredDmXUTipDo31Juda2Y10jfK9DYS0gBPAjD-X")
	jw := cell.BeginCell().MustStoreUInt(0xabcd, 16).EndCell()
	code := cell.BeginCell().MustStoreUInt(0x1234, 16).EndCell()
	var targets []*walletspam.WalletSpam
	for sub := uint32(0); len(targets) < n; sub++ {
		w := walletspam.New(sub, pub, priv, minter, jw, code)
		if Shard(w.Address()) == shard {
			targets = append(targets, w)
		}
	}
	action, err := gram.BuildTransferInternalWithComment(minter, tlb.MustFromTON("0.001"), strings.Repeat("x", gram.MaxCommentSize))
	if err != nil {
		t.Fatal(err)
	}
	return set, targets, action
}

func TestFourRelayersPreserveExistingAddresses(t *testing.T) {
	pub, err := hex.DecodeString("35c9c90be70363e8f343408cc48f4b6d4acfa987f21fa773f1499032ce57694e")
	if err != nil {
		t.Fatal(err)
	}
	set := NewShardSet(ed25519.PublicKey(pub), nil, DefaultTimeout)
	want := []string{
		"EQANpaxjolN9ftbLhugcTyH15twKeFl76b19xFyLbCZBESsU",
		"EQBPHJV3VlH8OSF35n9XCKBvIvLB98aI7TyD3ja7UosrttjE",
		"EQCJcVPP-bws1PFY6pDpPCoaLD_w1yrDTpSC0X9kuzkhR21t",
		"EQDHP3d-6CBM4FuCiaP3oev_KbpWJwWvmAOhlX2q6GvS1hpJ",
	}
	if len(set) != 4 {
		t.Fatal("production relayer count is not four")
	}
	for i, w := range set {
		if w.Address().StringRaw() != address.MustParseAddr(want[i]).StringRaw() {
			t.Fatalf("changed preserved relayer %d", i)
		}
		if Shard(w.Address()) != i {
			t.Fatalf("relayer %d is in the wrong quarter", i)
		}
	}
	for nibble := 0; nibble < 16; nibble++ {
		data := make([]byte, 32)
		data[0] = byte(nibble << 4)
		a := address.NewAddress(0, 0, data)
		if Shard(a) != nibble/4 {
			t.Fatalf("nibble %x routes to wrong relayer", nibble)
		}
	}
}

func TestSixteenRelayersReuseOriginalWalletsAndRouteEachNibble(t *testing.T) {
	pub, err := hex.DecodeString("35c9c90be70363e8f343408cc48f4b6d4acfa987f21fa773f1499032ce57694e")
	if err != nil {
		t.Fatal(err)
	}
	four := NewShardWallets(ed25519.PublicKey(pub), nil, DefaultTimeout, NumShards)
	sixteen := NewShardWallets(ed25519.PublicKey(pub), nil, DefaultTimeout, MaxShards)
	if len(four) != 4 || len(sixteen) != 16 {
		t.Fatal("wrong relayer count")
	}
	for i := 0; i < MaxShards; i++ {
		if got := int(sixteen[i].Address().Data()[0] >> 4); got != i {
			t.Fatalf("nibble %x uses address nibble %x", i, got)
		}
		data := make([]byte, 32)
		data[0] = byte(i << 4)
		if got := ShardFor(address.NewAddress(0, 0, data), MaxShards); got != i {
			t.Fatalf("nibble %x routed to %x", i, got)
		}
		if i%4 == 0 && four[i/4].Address().StringRaw() != sixteen[i].Address().StringRaw() {
			t.Fatalf("changed existing wallet at nibble %x", i)
		}
	}
}

func TestBatcherWithSixteenRelayersSelectsTargetNibble(t *testing.T) {
	pub, priv := testKey(t, 0x67)
	set := NewShardWallets(pub, priv, DefaultTimeout, MaxShards)
	minter := address.MustParseAddr("kQCy3cWuUXredDmXUTipDo31Juda2Y10jfK9DYS0gBPAjD-X")
	jw, code := cell.BeginCell().EndCell(), cell.BeginCell().EndCell()
	ctx, cancel := context.WithTimeout(context.Background(), 4*time.Second)
	defer cancel()
	var sent atomic.Int32
	b := NewBatcher(ctx, set, func(_ context.Context, addr *address.Address, body *cell.Cell) error {
		bucket := int(addr.Data()[0] >> 4)
		if set[bucket].Address().StringRaw() != addr.StringRaw() {
			t.Fatal("wrong highload")
		}
		messages := decodeBatch(t, set[bucket], body)
		if len(messages) != 1 {
			t.Fatalf("got %d messages", len(messages))
		}
		var msg tlb.InternalMessage
		if err := tlb.LoadFromCell(&msg, messages[0].BeginParse()); err != nil {
			t.Fatal(err)
		}
		if int(msg.DstAddr.Data()[0]>>4) != bucket {
			t.Fatal("target crossed shard bucket")
		}
		sent.Add(1)
		return nil
	}, BatchOptions{})
	defer b.Close()
	var targets []*walletspam.WalletSpam
	for id := uint32(0); len(targets) < MaxShards; id++ {
		w := walletspam.New(id, pub, priv, minter, jw, code)
		i := int(w.Address().Data()[0] >> 4)
		found := false
		for _, other := range targets {
			if int(other.Address().Data()[0]>>4) == i {
				found = true
				break
			}
		}
		if !found {
			targets = append(targets, w)
		}
	}
	action := cell.BeginCell().MustStoreUInt(0xabcd, 16).EndCell()
	var wg sync.WaitGroup
	for _, target := range targets {
		wg.Add(1)
		go func(w *walletspam.WalletSpam) {
			defer wg.Done()
			if err := b.Submit(ctx, w, 0, tlb.MustFromTON("0.002"), action); err != nil {
				t.Error(err)
			}
		}(target)
	}
	wg.Wait()
	if sent.Load() != MaxShards {
		t.Fatalf("sent through %d/%d relayers", sent.Load(), MaxShards)
	}
}

func decodeBatch(t *testing.T, w *HighloadWallet, ext *cell.Cell) []*cell.Cell {
	t.Helper()
	s := ext.BeginParse()
	sig := s.MustLoadSlice(512)
	payload := s.MustLoadRef().MustToCell()
	if !ed25519.Verify(w.pub, payload.Hash(), sig) {
		t.Fatal("invalid batch signature")
	}
	p := payload.BeginParse()
	if uint32(p.MustLoadUInt(32)) != w.subwalletID {
		t.Fatal("wrong subwallet")
	}
	var self tlb.InternalMessage
	if err := tlb.LoadFromCell(&self, p.MustLoadRef()); err != nil {
		t.Fatal(err)
	}
	if self.DstAddr.StringRaw() != w.Address().StringRaw() {
		t.Fatal("batch bypassed self-message")
	}
	if self.Amount.Nano().Uint64() != batchSelfValueNano {
		t.Fatal("self-message compute is not funded")
	}
	p.MustLoadUInt(8)
	queryID := p.MustLoadUInt(23)
	body := self.Body.BeginParse()
	if body.MustLoadUInt(32) != batchTransferOp || body.MustLoadUInt(64) != queryID {
		t.Fatal("invalid internal_transfer/query_id")
	}
	list := body.MustLoadRef()
	var messages []*cell.Cell
	for list.BitsLeft() > 0 {
		prev := list.MustLoadRef()
		if list.MustLoadUInt(32) != walletspam.ActionSendMsgOp || list.MustLoadUInt(8) != uint64(SendModeRelay) {
			t.Fatal("invalid relay action")
		}
		messages = append(messages, list.MustLoadRef().MustToCell())
		list = prev
	}
	return messages
}

func TestBatchBuilderAlwaysSelfMessage(t *testing.T) {
	set, targets, action := batchFixture(t, 3, 0)
	for _, n := range []int{1, 3} {
		var messages []*cell.Cell
		for _, target := range targets[:n] {
			msg, err := BuildRelayMessage(target, 7, 1786800120, tlb.MustFromTON("0.002"), action)
			if err != nil {
				t.Fatal(err)
			}
			messages = append(messages, msg)
		}
		ext, err := set[0].BuildBatchExternal(messages, 1786800000)
		if err != nil {
			t.Fatal(err)
		}
		got := decodeBatch(t, set[0], ext)
		if len(got) != n {
			t.Fatalf("got %d actions, want %d", len(got), n)
		}
		for i, msg := range got {
			if !bytes.Equal(msg.Hash(), messages[n-1-i].Hash()) {
				t.Fatal("batch changed a signed relay")
			}
		}
	}
}

func TestBatcherGroupsAndDropsCancelledRequests(t *testing.T) {
	set, targets, action := batchFixture(t, 3, 0)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	var count int
	b := NewBatcher(ctx, set[:], func(_ context.Context, _ *address.Address, ext *cell.Cell) error {
		msgs := decodeBatch(t, set[0], ext)
		count = len(msgs)
		for _, msg := range msgs {
			var m tlb.InternalMessage
			tlb.LoadFromCell(&m, msg.BeginParse())
			if m.DstAddr.StringRaw() == targets[2].Address().StringRaw() {
				t.Error("cancelled request was sent")
			}
		}
		return nil
	}, BatchOptions{NoRetry: func() bool { return true }})
	defer b.Close()
	results := make(chan error, 3)
	for _, target := range targets[:2] {
		go func(w *walletspam.WalletSpam) { results <- b.Submit(ctx, w, 1, tlb.MustFromTON("0.002"), action) }(target)
	}
	short, stop := context.WithTimeout(ctx, 50*time.Millisecond)
	defer stop()
	go func() { results <- b.Submit(short, targets[2], 1, tlb.MustFromTON("0.002"), action) }()
	ok, cancelled := 0, 0
	for i := 0; i < 3; i++ {
		err := <-results
		if err == nil {
			ok++
		} else if errors.Is(err, context.DeadlineExceeded) {
			cancelled++
		} else {
			t.Fatal(err)
		}
	}
	if ok != 2 || cancelled != 1 || count != 2 {
		t.Fatalf("ok=%d cancelled=%d batch size=%d", ok, cancelled, count)
	}
}

func TestBatcherRetryIsIdenticalAndPaced(t *testing.T) {
	set, targets, action := batchFixture(t, 1, 0)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	var times []time.Time
	var hashes [][]byte
	var dispatches int
	b := NewBatcher(ctx, set[:], func(_ context.Context, _ *address.Address, ext *cell.Cell) error {
		times = append(times, time.Now())
		hashes = append(hashes, ext.Hash())
		if len(times) == 1 {
			return errors.New("transport failed after submission")
		}
		return nil
	}, BatchOptions{OnDispatch: func(dst *address.Address, ext *cell.Cell, requests []context.Context) {
		dispatches++
		if !dst.Equals(set[0].Address()) || len(requests) != 1 || requests[0] != ctx || len(ext.Hash()) != 32 {
			t.Error("dispatch did not identify the batch and its sampled context")
		}
	}})
	defer b.Close()
	if err := b.Submit(ctx, targets[0], 4, tlb.MustFromTON("0.002"), action); err != nil {
		t.Fatal(err)
	}
	if len(times) != 2 || !bytes.Equal(hashes[0], hashes[1]) {
		t.Fatal("retry changed query_id or external body")
	}
	if dispatches != 1 {
		t.Fatalf("OnDispatch called %d times for one retried external", dispatches)
	}
	if gap := times[1].Sub(times[0]); gap < BatchInterval {
		t.Fatalf("retry bypassed limiter: gap %s", gap)
	}
}

func TestBatcherSplitsByWireSize(t *testing.T) {
	set, targets, _ := batchFixture(t, MaxBatchMessages, 0)
	ctx, cancel := context.WithTimeout(context.Background(), 6*time.Second)
	defer cancel()
	var batches, total int
	b := NewBatcher(ctx, set[:], func(_ context.Context, _ *address.Address, ext *cell.Cell) error {
		if size := len(set[0].externalWireCell(ext).ToBOC()); size > MaxBatchBytes {
			t.Errorf("oversized external: %d", size)
		}
		msgs := decodeBatch(t, set[0], ext)
		batches++
		total += len(msgs)
		return nil
	}, BatchOptions{NoRetry: func() bool { return true }})
	defer b.Close()
	var wg sync.WaitGroup
	for _, target := range targets {
		wg.Add(1)
		go func(w *walletspam.WalletSpam) {
			defer wg.Done()
			// Distinct recipients/comments prevent BoC deduplication from hiding the wire-size limit.
			uniqueAction, err := gram.BuildTransferInternalWithComment(w.Address(), tlb.MustFromTON("0.001"), fmt.Sprintf("%0123d", w.SubwalletID))
			if err != nil {
				t.Error(err)
				return
			}
			if err := b.Submit(ctx, w, 0, tlb.MustFromTON("0.002"), uniqueAction); err != nil {
				t.Error(err)
			}
		}(target)
	}
	wg.Wait()
	if batches < 2 || total != MaxBatchMessages {
		t.Fatalf("batches=%d relays=%d", batches, total)
	}
}

func TestBatcherCloseCancelsQueue(t *testing.T) {
	set, targets, action := batchFixture(t, 1, 0)
	ctx, cancel := context.WithCancel(context.Background())
	var calls atomicCounter
	b := NewBatcher(ctx, set[:], func(context.Context, *address.Address, *cell.Cell) error { calls.add(); return nil }, BatchOptions{})
	result := make(chan error, 1)
	go func() { result <- b.Submit(ctx, targets[0], 0, tlb.MustFromTON("0.002"), action) }()
	cancel()
	b.Close()
	if err := <-result; !errors.Is(err, context.Canceled) {
		t.Fatalf("got %v", err)
	}
	if calls.get() != 0 {
		t.Fatal("sent a batch after stop")
	}
}

// A mutex-backed counter keeps this shutdown assertion independent of callback timing.
type atomicCounter struct {
	mu sync.Mutex
	n  int
}

func (c *atomicCounter) add()     { c.mu.Lock(); c.n++; c.mu.Unlock() }
func (c *atomicCounter) get() int { c.mu.Lock(); defer c.mu.Unlock(); return c.n }
