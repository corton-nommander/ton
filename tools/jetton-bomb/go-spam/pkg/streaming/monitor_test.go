package streaming

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
)

type fakeStream struct {
	server  *httptest.Server
	mu      sync.Mutex
	conn    *websocket.Conn
	updates chan []string
}

func newFakeStream(t *testing.T) *fakeStream {
	t.Helper()
	f := &fakeStream{updates: make(chan []string, 20)}
	f.server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Query().Get("api_key") != "testkey" {
			http.Error(w, "unauthorized", 401)
			return
		}
		conn, err := (&websocket.Upgrader{}).Upgrade(w, r, nil)
		if err != nil {
			return
		}
		f.mu.Lock()
		f.conn = conn
		f.mu.Unlock()
		defer conn.Close()
		for {
			var req struct {
				ID        string   `json:"id"`
				Operation string   `json:"operation"`
				Addresses []string `json:"addresses"`
			}
			if conn.ReadJSON(&req) != nil {
				return
			}
			status := "subscribed"
			if req.Operation == "unsubscribe" {
				status = "unsubscribed"
			}
			if req.Operation == "ping" {
				status = "pong"
			}
			f.mu.Lock()
			_ = conn.WriteJSON(map[string]any{"id": req.ID, "status": status})
			f.mu.Unlock()
			if req.Operation != "ping" {
				f.updates <- req.Addresses
			}
		}
	}))
	t.Cleanup(func() { f.server.Close() })
	return f
}

func (f *fakeStream) emit(t *testing.T, event map[string]any) {
	t.Helper()
	f.mu.Lock()
	defer f.mu.Unlock()
	if err := f.conn.WriteJSON(event); err != nil {
		t.Fatal(err)
	}
}

func eventually(t *testing.T, want func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if want() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("condition never became true")
}

func testAddress(n byte) *address.Address {
	b := make([]byte, 32)
	b[31] = n
	return address.NewAddress(0, 0, b)
}

func TestConfirmedDirectionAndTrace(t *testing.T) {
	f := newFakeStream(t)
	memory := NewMemory()
	m := New("ws"+strings.TrimPrefix(f.server.URL, "http"), "testkey", 2, memory, nil)
	t.Cleanup(m.Close)
	m.On()
	eventually(t, m.Ready)
	sender, recipient := testAddress(1), testAddress(2)
	intent := Intent{RunID: 12, Mode: "gram", Kind: "ton_transfer", Sender: sender.StringRaw(), Recipient: recipient.StringRaw()}
	s := m.TrySample(context.Background(), intent)
	if s == nil {
		t.Fatal("sample not subscribed")
	}
	select {
	case addrs := <-f.updates:
		if len(addrs) != 1 || addrs[0] != intent.Recipient {
			t.Fatalf("subscribed to %v", addrs)
		}
	case <-time.After(time.Second):
		t.Fatal("no subscribe")
	}
	body := cell.BeginCell().MustStoreUInt(7, 32).EndCell()
	m.Bind(s, sender, body)
	hash := backend.ExternalHashNorm(sender, body)
	action := func(from, to string) map[string]any {
		return map[string]any{"type": "ton_transfer", "success": true, "details": map[string]any{"source": from, "destination": to}}
	}
	f.emit(t, map[string]any{"type": "actions", "finality": "confirmed", "trace_external_hash_norm": "other", "actions": []any{action(intent.Sender, intent.Recipient)}})
	f.emit(t, map[string]any{"type": "actions", "finality": "confirmed", "trace_external_hash_norm": hash, "actions": []any{action(intent.Recipient, intent.Sender)}})
	if memory.Snapshot(12).Confirmed != 0 {
		t.Fatal("matched unrelated action")
	}
	f.emit(t, map[string]any{"type": "actions", "finality": "confirmed", "trace_external_hash_norm": hash, "actions": []any{action(intent.Sender, intent.Recipient)}})
	eventually(t, func() bool { return memory.Snapshot(12).Confirmed == 1 })
	if got := m.AddressCount(); got != 1 {
		t.Fatalf("subscription dropped before finalized: %d addresses", got)
	}
	f.emit(t, map[string]any{"type": "actions", "finality": "confirmed", "trace_external_hash_norm": hash, "actions": []any{action(intent.Sender, intent.Recipient)}})
	f.emit(t, map[string]any{"type": "actions", "finality": "finalized", "trace_external_hash_norm": hash, "actions": []any{action(intent.Sender, intent.Recipient)}})
	eventually(t, func() bool { return memory.Snapshot(12).Finalized == 1 })
	if got := memory.Snapshot(12).Confirmed; got != 1 {
		t.Fatalf("confirmed counted twice: %d", got)
	}
	eventually(t, func() bool { return m.AddressCount() == 0 })
	f.emit(t, map[string]any{"type": "actions", "finality": "finalized", "trace_external_hash_norm": hash, "actions": []any{action(intent.Sender, intent.Recipient)}})
	if got := memory.Snapshot(12).Finalized; got != 1 {
		t.Fatalf("finalized counted twice: %d", got)
	}
}

func TestGaslessParentAndRepeatedRecipient(t *testing.T) {
	f := newFakeStream(t)
	memory := NewMemory()
	m := New("ws"+strings.TrimPrefix(f.server.URL, "http"), "testkey", 1, memory, nil)
	t.Cleanup(m.Close)
	m.On()
	eventually(t, m.Ready)
	sender, recipient := testAddress(3), testAddress(4)
	intent := Intent{RunID: 13, Kind: "jetton_transfer", Mode: "gassless_jetton", Sender: sender.StringRaw(), Recipient: recipient.StringRaw(), Asset: testAddress(9).StringRaw(), Gasless: true}
	s1 := m.TrySample(context.Background(), intent)
	if s1 == nil {
		t.Fatal("first sample")
	}
	m.lastSample.Store(0)
	s2 := m.TrySample(context.Background(), intent)
	if s2 == nil || m.AddressCount() != 1 {
		t.Fatal("duplicate address consumed second slot")
	}
	relayer := testAddress(5)
	body := cell.BeginCell().MustStoreUInt(42, 32).EndCell()
	m.Bind(s1, relayer, body)
	m.Bind(s2, relayer, body)
	hash := backend.ExternalHashNorm(relayer, body)
	gasless := map[string]any{"action_id": "parent", "type": "gasless_request", "success": true, "details": map[string]any{"destination": intent.Sender}}
	transfer := map[string]any{"type": "jetton_transfer", "success": true, "parent_gasless_action": "parent", "details": map[string]any{"sender": intent.Sender, "receiver": intent.Recipient, "asset": intent.Asset}}
	f.emit(t, map[string]any{"type": "actions", "finality": "confirmed", "trace_external_hash_norm": hash, "actions": []any{gasless, transfer}})
	time.Sleep(30 * time.Millisecond)
	if memory.Snapshot(13).Confirmed != 0 {
		t.Fatal("ambiguous transfer attributed to one sample")
	}
	m.EndAttempt(s1) // both are dispatched; uncertainty is retained until timeout/off
	m.Off()
	if got := memory.Snapshot(13); got.Interrupted != 2 || got.Confirmed != 0 {
		t.Fatalf("unexpected snapshot: %+v", got)
	}
}

func TestGaslessParentRequiresRightRequest(t *testing.T) {
	f := newFakeStream(t)
	memory := NewMemory()
	m := New("ws"+strings.TrimPrefix(f.server.URL, "http"), "testkey", 2, memory, nil)
	t.Cleanup(m.Close)
	m.On()
	eventually(t, m.Ready)
	from, to, relayer := testAddress(1), testAddress(2), testAddress(3)
	intent := Intent{RunID: 33, Mode: "gassless_gram", Kind: "ton_transfer", Sender: from.StringRaw(), Recipient: to.StringRaw(), Gasless: true}
	s := m.TrySample(context.Background(), intent)
	if s == nil {
		t.Fatal("sample")
	}
	body := cell.BeginCell().MustStoreUInt(42, 32).EndCell()
	m.Bind(s, relayer, body)
	transfer := map[string]any{"type": "ton_transfer", "success": true, "parent_gasless_action": "parent",
		"details": map[string]any{"source": intent.Sender, "destination": intent.Recipient}}
	parent := map[string]any{"type": "gasless_request", "success": true, "action_id": "parent",
		"details": map[string]any{"destination": testAddress(4).StringRaw()}}
	event := map[string]any{"type": "actions", "finality": "confirmed", "trace_external_hash_norm": backend.ExternalHashNorm(relayer, body), "actions": []any{parent, transfer}}
	f.emit(t, event)
	time.Sleep(30 * time.Millisecond)
	if memory.Snapshot(33).Confirmed != 0 {
		t.Fatal("wrong parent matched")
	}
	parent["details"] = map[string]any{"destination": intent.Sender}
	f.emit(t, event)
	eventually(t, func() bool { return memory.Snapshot(33).Confirmed == 1 })
}

func TestGaslessRecipientSubscriptionCanOmitParentAction(t *testing.T) {
	f := newFakeStream(t)
	mem := NewMemory()
	m := New("ws"+strings.TrimPrefix(f.server.URL, "http"), "testkey", 1, mem, nil)
	t.Cleanup(m.Close)
	m.On()
	eventually(t, m.Ready)
	from, to, relayer := testAddress(1), testAddress(2), testAddress(3)
	intent := Intent{RunID: 44, Kind: "ton_transfer", Sender: from.StringRaw(), Recipient: to.StringRaw(), Gasless: true}
	s := m.TrySample(context.Background(), intent)
	if s == nil {
		t.Fatal("subscribe failed")
	}
	body := cell.BeginCell().MustStoreUInt(99, 32).EndCell()
	m.Bind(s, relayer, body)
	transfer := map[string]any{"type": "ton_transfer", "success": true, "parent_gasless_action": "parent-only-on-target",
		"details": map[string]any{"source": intent.Sender, "destination": intent.Recipient}}
	event := map[string]any{"type": "actions", "finality": "confirmed", "trace_external_hash_norm": backend.ExternalHashNorm(relayer, body),
		"actions": []any{transfer}}
	f.emit(t, event)
	eventually(t, func() bool { return mem.Snapshot(44).Confirmed == 1 })
	if got := m.Pending(44); got != 1 {
		t.Fatalf("sample no longer awaiting finalization: %d", got)
	}
	event["finality"] = "finalized"
	f.emit(t, event)
	eventually(t, func() bool { return mem.Snapshot(44).Finalized == 1 && m.Pending(44) == 0 })
}

func TestTwoIdenticalTransfersInOneTraceAreAmbiguous(t *testing.T) {
	f := newFakeStream(t)
	mem := NewMemory()
	m := New("ws"+strings.TrimPrefix(f.server.URL, "http"), "testkey", 1, mem, nil)
	t.Cleanup(m.Close)
	m.On()
	eventually(t, m.Ready)
	from, to, relayer := testAddress(1), testAddress(2), testAddress(3)
	intent := Intent{RunID: 45, Kind: "ton_transfer", Sender: from.StringRaw(), Recipient: to.StringRaw(), Gasless: true}
	s := m.TrySample(context.Background(), intent)
	if s == nil {
		t.Fatal("subscribe failed")
	}
	body := cell.BeginCell().MustStoreUInt(100, 32).EndCell()
	m.Bind(s, relayer, body)
	action := func(parent string) map[string]any {
		return map[string]any{"type": "ton_transfer", "success": true, "parent_gasless_action": parent,
			"details": map[string]any{"source": intent.Sender, "destination": intent.Recipient}}
	}
	f.emit(t, map[string]any{"type": "actions", "finality": "confirmed", "trace_external_hash_norm": backend.ExternalHashNorm(relayer, body),
		"actions": []any{action("first"), action("second")}})
	time.Sleep(30 * time.Millisecond)
	if mem.Snapshot(45).Confirmed != 0 {
		t.Fatal("picked a random matching transfer")
	}
}

func TestCapAndTimeout(t *testing.T) {
	f := newFakeStream(t)
	memory := NewMemory()
	m := New("ws"+strings.TrimPrefix(f.server.URL, "http"), "testkey", 1, memory, nil)
	t.Cleanup(m.Close)
	m.On()
	eventually(t, m.Ready)
	first := m.TrySample(context.Background(), Intent{RunID: 5, Recipient: testAddress(1).StringRaw()})
	if first == nil {
		t.Fatal("first sample")
	}
	second := m.TrySample(context.Background(), Intent{RunID: 5, Recipient: testAddress(2).StringRaw()})
	if second != nil || memory.Snapshot(5).NoSlot != 1 {
		t.Fatal("capacity exceeded")
	}
	m.mu.Lock()
	first.started = time.Now().Add(-Lifetime - time.Second)
	m.mu.Unlock()
	m.expire()
	if got := memory.Snapshot(5); got.Missing != 1 || m.Pending(5) != 0 {
		t.Fatalf("timeout: %+v", got)
	}
}

func TestConfirmedSampleTimesOutWaitingForFinalized(t *testing.T) {
	f := newFakeStream(t)
	memory := NewMemory()
	m := New("ws"+strings.TrimPrefix(f.server.URL, "http"), "testkey", 1, memory, nil)
	t.Cleanup(m.Close)
	m.On()
	eventually(t, m.Ready)
	sender, recipient := testAddress(1), testAddress(2)
	intent := Intent{RunID: 6, Kind: "ton_transfer", Sender: sender.StringRaw(), Recipient: recipient.StringRaw()}
	s := m.TrySample(context.Background(), intent)
	if s == nil {
		t.Fatal("subscribe failed")
	}
	body := cell.BeginCell().MustStoreUInt(7, 32).EndCell()
	m.Bind(s, sender, body)
	f.emit(t, map[string]any{"type": "actions", "finality": "confirmed", "trace_external_hash_norm": backend.ExternalHashNorm(sender, body),
		"actions": []any{map[string]any{"type": "ton_transfer", "success": true,
			"details": map[string]any{"source": intent.Sender, "destination": intent.Recipient}}}})
	eventually(t, func() bool { return memory.Snapshot(6).Confirmed == 1 })
	m.mu.Lock()
	s.started = time.Now().Add(-Lifetime - time.Second)
	m.mu.Unlock()
	m.expire()
	got := memory.Snapshot(6)
	if got.Confirmed != 1 || got.Finalized != 0 || got.Missing != 0 || got.FinalizedMissing != 1 || m.Pending(6) != 0 {
		t.Fatalf("confirmed/finalized timeout accounted incorrectly: %+v", got)
	}
}

func TestFinalizedSnapshotWithoutConfirmedDoesNotInventConfirmation(t *testing.T) {
	f := newFakeStream(t)
	memory := NewMemory()
	m := New("ws"+strings.TrimPrefix(f.server.URL, "http"), "testkey", 1, memory, nil)
	t.Cleanup(m.Close)
	m.On()
	eventually(t, m.Ready)
	sender, recipient := testAddress(1), testAddress(2)
	intent := Intent{RunID: 7, Kind: "ton_transfer", Sender: sender.StringRaw(), Recipient: recipient.StringRaw()}
	s := m.TrySample(context.Background(), intent)
	if s == nil {
		t.Fatal("subscribe failed")
	}
	body := cell.BeginCell().MustStoreUInt(8, 32).EndCell()
	m.Bind(s, sender, body)
	f.emit(t, map[string]any{"type": "actions", "finality": "finalized", "trace_external_hash_norm": backend.ExternalHashNorm(sender, body),
		"actions": []any{map[string]any{"type": "ton_transfer", "success": true,
			"details": map[string]any{"source": intent.Sender, "destination": intent.Recipient}}}})
	eventually(t, func() bool { return memory.Snapshot(7).Finalized == 1 })
	if got := memory.Snapshot(7); got.Confirmed != 0 || got.Missing != 0 || m.Pending(7) != 0 {
		t.Fatalf("finalized-only event created a synthetic confirmation: %+v", got)
	}
}

func TestResultJSONStorageBoundary(t *testing.T) {
	r := Result{Intent: Intent{RunID: 1, Kind: "ton_transfer"}, Outcome: "not_observed"}
	b, err := json.Marshal(r)
	if err != nil || !strings.Contains(string(b), "not_observed") {
		t.Fatalf("record: %s %v", b, err)
	}
}
