// Package streaming observes a bounded, in-memory sample of confirmed transfer
// actions. No RPC or database operation is performed on the bulk send path.
package streaming

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
)

const Lifetime = 90 * time.Second

type Intent struct {
	RunID     int64
	Mode      string
	Kind      string // ton_transfer or jetton_transfer
	Sender    string // raw address
	Recipient string // raw address
	Asset     string // raw minter address, for jettons
	Gasless   bool
}

// Result is the storage boundary. A future SQLite/ClickHouse writer can accept
// these records without changing the sampler or the sender. Record must not block.
type Result struct {
	Intent
	SampleID  uint64
	StartedAt time.Time
	EndedAt   time.Time
	Latency   time.Duration
	Trace     string
	Outcome   string // confirmed, finalized, not_observed, finalized_not_observed, interrupted, send_failed
}

type Sink interface{ Record(Result) }

type Snapshot struct {
	Confirmed, Finalized, Missing, FinalizedMissing, Interrupted, SendFailed, Pending, NoSlot uint64
	P50, P80, P95, P99                                                                        time.Duration
	FinalizedP50, FinalizedP80, FinalizedP95, FinalizedP99                                    time.Duration
}

// Memory stores only bounded recent latencies and per-run counters.
type Memory struct {
	mu   sync.Mutex
	runs map[int64]*runStats
}

type runStats struct {
	confirmed, finalized, missing, finalizedMissing, interrupted, sendFailed, noSlot uint64
	latencies, finalizedLatencies                                                    []time.Duration
}

func NewMemory() *Memory { return &Memory{runs: make(map[int64]*runStats)} }

func (m *Memory) run(id int64) *runStats {
	r := m.runs[id]
	if r == nil {
		if len(m.runs) >= 32 {
			var oldest int64
			first := true
			for key := range m.runs {
				if first || key < oldest {
					oldest, first = key, false
				}
			}
			delete(m.runs, oldest)
		}
		r = &runStats{}
		m.runs[id] = r
	}
	return r
}

func (m *Memory) Record(v Result) {
	m.mu.Lock()
	defer m.mu.Unlock()
	r := m.run(v.RunID)
	switch v.Outcome {
	case "confirmed":
		r.confirmed++
		r.latencies = append(r.latencies, v.Latency)
		if len(r.latencies) > 3600 {
			r.latencies = r.latencies[len(r.latencies)-3600:]
		}
	case "finalized":
		r.finalized++
		r.finalizedLatencies = append(r.finalizedLatencies, v.Latency)
		if len(r.finalizedLatencies) > 3600 {
			r.finalizedLatencies = r.finalizedLatencies[len(r.finalizedLatencies)-3600:]
		}
	case "not_observed":
		r.missing++
	case "finalized_not_observed":
		r.finalizedMissing++
	case "interrupted":
		r.interrupted++
	case "send_failed":
		r.sendFailed++
	}
}

func (m *Memory) NoSlot(runID int64) {
	m.mu.Lock()
	m.run(runID).noSlot++
	m.mu.Unlock()
}

func (m *Memory) Snapshot(runID int64) Snapshot {
	m.mu.Lock()
	r := m.run(runID)
	s := Snapshot{Confirmed: r.confirmed, Finalized: r.finalized, Missing: r.missing,
		FinalizedMissing: r.finalizedMissing, Interrupted: r.interrupted, SendFailed: r.sendFailed, NoSlot: r.noSlot}
	confirmed := append([]time.Duration(nil), r.latencies...)
	finalized := append([]time.Duration(nil), r.finalizedLatencies...)
	m.mu.Unlock()
	s.P50, s.P80, s.P95, s.P99 = percentiles(confirmed)
	s.FinalizedP50, s.FinalizedP80, s.FinalizedP95, s.FinalizedP99 = percentiles(finalized)
	return s
}

func percentiles(d []time.Duration) (p50, p80, p95, p99 time.Duration) {
	if len(d) == 0 {
		return
	}
	sort.Slice(d, func(i, j int) bool { return d[i] < d[j] })
	at := func(p float64) time.Duration { return d[int(math.Ceil(float64(len(d))*p))-1] }
	return at(.5), at(.8), at(.95), at(.99)
}

type Sample struct {
	Intent
	started        time.Time
	traces         map[string]bool
	id             uint64
	confirmedTrace string
}

type Monitor struct {
	url, key   string
	max        int
	sink       Sink
	memory     *Memory
	enabled    atomic.Bool
	ready      atomic.Bool
	mu         sync.Mutex
	writeMu    sync.Mutex
	subMu      sync.Mutex
	conn       *websocket.Conn
	acks       map[string]chan bool
	pending    map[uint64]*Sample
	addresses  map[string]int
	subscribed map[string]bool
	seq        uint64
	lastSample atomic.Int64
	lastPong   atomic.Int64
	ctx        context.Context
	cancel     context.CancelFunc
}

func New(url, key string, max int, memory *Memory, extra Sink) *Monitor {
	if max < 1 || max > 90 {
		max = 90
	}
	ctx, cancel := context.WithCancel(context.Background())
	var sink Sink = memory
	if extra != nil {
		sink = combinedSink{memory, extra}
	}
	m := &Monitor{url: url, key: key, max: max, memory: memory, sink: sink,
		acks: make(map[string]chan bool), pending: make(map[uint64]*Sample),
		addresses: make(map[string]int), subscribed: make(map[string]bool), ctx: ctx, cancel: cancel}
	go m.run()
	return m
}

type combinedSink struct{ a, b Sink }

func (s combinedSink) Record(r Result) { s.a.Record(r); s.b.Record(r) }

func (m *Monitor) Enabled() bool { return m.enabled.Load() }
func (m *Monitor) Ready() bool   { return m.ready.Load() }
func (m *Monitor) Limit() int    { return m.max }
func (m *Monitor) Close()        { m.cancel(); m.Off() }

// Claim is the cheap per-send check. Only its winner derives a sample address.
func (m *Monitor) Claim() bool {
	if !m.enabled.Load() || !m.ready.Load() {
		return false
	}
	now := time.Now().UnixNano()
	for {
		old := m.lastSample.Load()
		if now-old < int64(time.Second) {
			return false
		}
		if m.lastSample.CompareAndSwap(old, now) {
			return true
		}
	}
}
func (m *Monitor) On() {
	if m.key != "" {
		m.enabled.Store(true)
	}
}

func (m *Monitor) Off() {
	m.enabled.Store(false)
	m.ready.Store(false)
	m.mu.Lock()
	c := m.conn
	m.conn = nil
	m.subscribed = make(map[string]bool)
	for _, s := range m.pending {
		m.finishLocked(s, "interrupted", "", time.Now())
	}
	m.mu.Unlock()
	if c != nil {
		_ = c.Close()
	}
}

func (m *Monitor) run() {
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		select {
		case <-m.ctx.Done():
			return
		case <-tick.C:
		}
		m.expire()
		if m.ready.Load() && time.Since(time.Unix(0, m.lastPong.Load())) > 45*time.Second {
			m.mu.Lock()
			c := m.conn
			m.mu.Unlock()
			if c != nil {
				_ = c.Close()
			}
			continue
		}
		if m.ready.Load() && time.Now().Unix()%15 == 0 {
			m.mu.Lock()
			c := m.conn
			m.mu.Unlock()
			if c != nil {
				m.writeMu.Lock()
				_ = c.SetWriteDeadline(time.Now().Add(time.Second))
				_ = c.WriteJSON(map[string]string{"operation": "ping", "id": "keepalive"})
				m.writeMu.Unlock()
			}
		}
		if !m.enabled.Load() || m.ready.Load() {
			continue
		}
		ctx, cancel := context.WithTimeout(m.ctx, 5*time.Second)
		c, _, err := websocket.DefaultDialer.DialContext(ctx, m.url+"?api_key="+m.key, nil)
		cancel()
		if err != nil {
			continue
		} // errors can contain the key-bearing URL
		m.mu.Lock()
		if !m.enabled.Load() {
			m.mu.Unlock()
			_ = c.Close()
			continue
		}
		m.conn = c
		m.ready.Store(true)
		m.lastPong.Store(time.Now().UnixNano())
		m.mu.Unlock()
		go m.read(c)
	}
}

func (m *Monitor) read(c *websocket.Conn) {
	for {
		_, raw, err := c.ReadMessage()
		if err != nil {
			break
		}
		var event struct {
			ID       string          `json:"id"`
			Status   string          `json:"status"`
			Error    json.RawMessage `json:"error"`
			Type     string          `json:"type"`
			Finality string          `json:"finality"`
			Trace    string          `json:"trace_external_hash_norm"`
			Actions  []action        `json:"actions"`
		}
		if json.Unmarshal(raw, &event) != nil {
			continue
		}
		m.mu.Lock()
		if c != m.conn {
			m.mu.Unlock()
			break
		}
		if ack := m.acks[event.ID]; ack != nil {
			delete(m.acks, event.ID)
			ack <- (event.Status == "subscribed" || event.Status == "unsubscribed") && len(event.Error) == 0
		}
		if event.Status == "pong" {
			m.lastPong.Store(time.Now().UnixNano())
		}
		if event.Type == "actions" && (event.Finality == "confirmed" || event.Finality == "finalized") {
			m.matchLocked(event.Trace, event.Actions, event.Finality)
		}
		m.mu.Unlock()
	}
	m.mu.Lock()
	if m.conn == c {
		m.conn = nil
		m.subscribed = make(map[string]bool)
		m.ready.Store(false)
		for _, s := range m.pending {
			m.finishLocked(s, "interrupted", "", time.Now())
		}
	}
	m.mu.Unlock()
	_ = c.Close()
}

type action struct {
	ID      string `json:"action_id"`
	Type    string `json:"type"`
	Success bool   `json:"success"`
	Parent  string `json:"parent_gasless_action"`
	Details struct {
		Source      string `json:"source"`
		Destination string `json:"destination"`
		Sender      string `json:"sender"`
		Receiver    string `json:"receiver"`
		Asset       string `json:"asset"`
	} `json:"details"`
}

func (m *Monitor) matchLocked(trace string, actions []action, finality string) {
	parents := make(map[string]string)
	counts := make(map[string]int)
	for _, a := range actions {
		if a.Type == "gasless_request" && a.Success {
			parents[a.ID] = strings.ToLower(a.Details.Destination)
		}
		if a.Type == "ton_transfer" || a.Type == "jetton_transfer" {
			counts[actionKey(a)]++
		}
	}
	for _, a := range actions {
		if !a.Success || (a.Type != "ton_transfer" && a.Type != "jetton_transfer") {
			continue
		}
		if counts[actionKey(a)] != 1 {
			continue
		} // two indistinguishable transfers in this batch
		from, to := a.Details.Source, a.Details.Destination
		if a.Type == "jetton_transfer" {
			from, to = a.Details.Sender, a.Details.Receiver
		}
		var matched *Sample
		matches := 0
		for _, s := range m.pending {
			if !s.traces[trace] || s.Kind != a.Type || !strings.EqualFold(s.Sender, from) || !strings.EqualFold(s.Recipient, to) {
				continue
			}
			if finality == "finalized" && s.confirmedTrace != "" && s.confirmedTrace != trace {
				continue
			}
			if s.Asset != "" && !strings.EqualFold(s.Asset, a.Details.Asset) {
				continue
			}
			if s.Gasless {
				if a.Parent == "" {
					continue
				}
				// Address-filtered streams may omit the gasless_request action
				// when only the recipient is subscribed. If it IS present,
				// validate the parent points to the sampled sender.
				if target, ok := parents[a.Parent]; ok && target != strings.ToLower(s.Sender) {
					continue
				}
			}
			matches++
			matched = s
		}
		if matches == 1 {
			now := time.Now()
			if finality == "confirmed" && matched.confirmedTrace == "" {
				matched.confirmedTrace = trace
				m.recordLocked(matched, "confirmed", trace, now)
			} else if finality == "finalized" {
				m.finishLocked(matched, "finalized", trace, now)
				go m.syncSnapshot(context.Background())
			}
		}
	}
}

func actionKey(a action) string {
	from, to := a.Details.Source, a.Details.Destination
	if a.Type == "jetton_transfer" {
		from, to = a.Details.Sender, a.Details.Receiver
	}
	return strings.ToLower(a.Type + "|" + from + "|" + to + "|" + a.Details.Asset)
}

func (m *Monitor) expire() {
	m.mu.Lock()
	changed := false
	now := time.Now()
	for _, s := range m.pending {
		if now.Sub(s.started) >= Lifetime {
			outcome := "not_observed"
			if s.confirmedTrace != "" {
				outcome = "finalized_not_observed"
			}
			m.finishLocked(s, outcome, "", now)
			changed = true
		}
	}
	m.mu.Unlock()
	if changed {
		go m.syncSnapshot(context.Background())
	}
}

func (m *Monitor) finishLocked(s *Sample, outcome, trace string, now time.Time) {
	if m.pending[s.id] != s {
		return
	}
	m.recordLocked(s, outcome, trace, now)
	delete(m.pending, s.id)
	if m.addresses[s.Recipient] <= 1 {
		delete(m.addresses, s.Recipient)
	} else {
		m.addresses[s.Recipient]--
	}
}

func (m *Monitor) recordLocked(s *Sample, outcome, trace string, now time.Time) {
	m.sink.Record(Result{Intent: s.Intent, SampleID: s.id, StartedAt: s.started,
		EndedAt: now, Latency: now.Sub(s.started), Trace: trace, Outcome: outcome})
}

// TrySample reserves a recipient and waits for its subscription ACK. The caller
// skips measurement (but still sends normally) on timeout, disconnect or cap.
func (m *Monitor) TrySample(ctx context.Context, intent Intent) *Sample {
	if !m.enabled.Load() || !m.ready.Load() {
		return nil
	}
	m.mu.Lock()
	if len(m.addresses) >= m.max && m.addresses[intent.Recipient] == 0 {
		m.memory.NoSlot(intent.RunID)
		m.mu.Unlock()
		return nil
	}
	m.seq++
	s := &Sample{Intent: intent, started: time.Now(), id: m.seq, traces: make(map[string]bool)}
	m.pending[s.id] = s
	m.addresses[s.Recipient]++
	m.mu.Unlock()
	ackCtx, cancel := context.WithTimeout(ctx, 250*time.Millisecond)
	defer cancel()
	if err := m.syncSnapshot(ackCtx); err != nil {
		m.mu.Lock()
		m.finishLocked(s, "interrupted", "", time.Now())
		m.mu.Unlock()
		go m.syncSnapshot(context.Background())
		return nil
	}
	m.mu.Lock()
	valid := m.enabled.Load() && m.pending[s.id] == s
	m.mu.Unlock()
	if !valid {
		return nil
	}
	return s
}

func (m *Monitor) syncSnapshot(ctx context.Context) error {
	ctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	m.subMu.Lock()
	defer m.subMu.Unlock()
	m.mu.Lock()
	c := m.conn
	if c == nil {
		m.mu.Unlock()
		return errors.New("stream offline")
	}
	addresses := make([]string, 0, len(m.addresses))
	for addr := range m.addresses {
		addresses = append(addresses, addr)
	}
	sort.Strings(addresses)
	if len(addresses) == 0 && len(m.subscribed) == 0 {
		m.mu.Unlock()
		return nil
	}
	operation := "subscribe"
	if len(addresses) == 0 {
		operation = "unsubscribe"
		for a := range m.subscribed {
			addresses = append(addresses, a)
		}
	}
	m.seq++
	id := fmt.Sprintf("sub-%d", m.seq)
	ack := make(chan bool, 1)
	m.acks[id] = ack
	m.mu.Unlock()
	defer func() { m.mu.Lock(); delete(m.acks, id); m.mu.Unlock() }()
	msg := map[string]any{"operation": operation, "id": id, "addresses": addresses}
	if operation == "subscribe" {
		msg["types"] = []string{"actions"}
		msg["action_types"] = []string{"ton_transfer", "jetton_transfer", "gasless_request"}
		msg["min_finality"] = "confirmed"
		msg["include_metadata"] = false
		msg["include_address_book"] = false
	}
	m.writeMu.Lock()
	_ = c.SetWriteDeadline(time.Now().Add(time.Second))
	err := c.WriteJSON(msg)
	m.writeMu.Unlock()
	if err != nil {
		_ = c.Close()
		return err
	}
	select {
	case ok := <-ack:
		if ok {
			m.mu.Lock()
			if m.conn == c {
				m.subscribed = make(map[string]bool, len(addresses))
				if operation == "subscribe" {
					for _, a := range addresses {
						m.subscribed[a] = true
					}
				}
			}
			m.mu.Unlock()
			return nil
		}
		return errors.New("subscribe rejected")
	case <-ctx.Done():
		return ctx.Err()
	}
}

// Bind associates an attempted external (including retries) with the sample.
func (m *Monitor) Bind(s *Sample, dst *address.Address, body *cell.Cell) {
	if s == nil {
		return
	}
	h := backend.ExternalHashNorm(dst, body)
	m.mu.Lock()
	if m.pending[s.id] == s {
		s.traces[h] = true
	}
	m.mu.Unlock()
}

// EndAttempt distinguishes a send that never built/dispatched an external from
// an uncertain RPC result. Once bound, it remains observable for 90 seconds.
func (m *Monitor) EndAttempt(s *Sample) {
	if s == nil {
		return
	}
	m.mu.Lock()
	if m.pending[s.id] == s && len(s.traces) == 0 {
		m.finishLocked(s, "send_failed", "", time.Now())
	}
	m.mu.Unlock()
}

func (m *Monitor) Pending(runID int64) int {
	m.mu.Lock()
	defer m.mu.Unlock()
	n := 0
	for _, s := range m.pending {
		if s.RunID == runID {
			n++
		}
	}
	return n
}

func (m *Monitor) AddressCount() int { m.mu.Lock(); defer m.mu.Unlock(); return len(m.addresses) }
