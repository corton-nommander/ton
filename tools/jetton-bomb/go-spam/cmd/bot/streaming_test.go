package main

import (
	"strings"
	"testing"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
	"github.com/victor/jetton-bomb-go-spam/pkg/streaming"
)

func TestDeliverySpoilerShowsP80BeforeErrors(t *testing.T) {
	memory := streaming.NewMemory()
	for i := 1; i <= 10; i++ {
		memory.Record(streaming.Result{
			Intent:  streaming.Intent{RunID: 1},
			Outcome: "confirmed",
			Latency: time.Duration(i) * time.Second,
		})
		memory.Record(streaming.Result{
			Intent:  streaming.Intent{RunID: 1},
			Outcome: "finalized",
			Latency: time.Duration(i*2) * time.Second,
		})
	}
	stats := memory.Snapshot(1)
	stats.Pending, stats.Missing = 2, 3
	message := formatRequestsBreakdownHTML(Snap{}, Snap{}, 30) + formatDeliveryHTML(stats) +
		FormatBreakdownHTML([]ErrCount{{Key: "send: failed", Count: 1}}, 1, 30)
	if !strings.Contains(message, "<summary>delivery <b>p80 8.00s</b></summary>") {
		t.Fatalf("p80 headline missing: %s", message)
	}
	if !strings.Contains(message, "<table bordered striped>") ||
		!strings.Contains(message, "<tr><td>not found</td><td>3</td>") ||
		!strings.Contains(message, "<tr><td>p95</td><td>10.00s</td>") ||
		!strings.Contains(message, "<tr><td>finalized</td><td>10</td>") ||
		!strings.Contains(message, "<tr><td>finalized p80</td><td>16.00s</td>") ||
		strings.Index(message, "<tr><td>finalized</td>") < strings.Index(message, "<tr><td>p99</td>") {
		t.Fatalf("delivery table missing metrics: %s", message)
	}
	if requests, delivery, errors := strings.Index(message, "<summary>requests"),
		strings.Index(message, "<summary>delivery"), strings.Index(message, "<summary>errors"); requests < 0 || delivery <= requests || errors <= delivery {
		t.Fatalf("spoilers out of order: %s", message)
	}
}

func TestStreamingToggleControlsChatAndEditedAuditDelivery(t *testing.T) {
	memory := streaming.NewMemory()
	memory.Record(streaming.Result{
		Intent: streaming.Intent{RunID: 1}, Outcome: "confirmed", Latency: 2 * time.Second,
	})
	memory.Record(streaming.Result{
		Intent: streaming.Intent{RunID: 1}, Outcome: "finalized", Latency: 3 * time.Second,
	})
	monitor := streaming.New("ws://127.0.0.1:0", "test-key", 1, memory, nil)
	defer monitor.Close()
	r := &Runtime{stream: monitor, streamMemory: memory}
	run := &SpamRun{startedAt: time.Unix(0, 1), auditHeader: "start gram spam"}
	stats := &pool.Stats{}
	if got := r.streamLine(run); got != "" {
		t.Fatalf("streaming off still shows delivery spoiler: %s", got)
	}
	if got := r.auditTransferStatus(run, stats, 0); strings.Contains(got, "delivery") {
		t.Fatalf("streaming off still shows edited audit delivery: %s", got)
	}
	monitor.On()
	if got := r.streamLine(run); !strings.Contains(got, "<summary>delivery <b>p80 2.00s</b></summary>") {
		t.Fatalf("streaming on missing delivery spoiler: %s", got)
	}
	if got := r.auditTransferStatus(run, stats, 0); !strings.HasSuffix(got,
		"\nconfirmed p50 2.00s, p80 2.00s, p99 2.00s\nfinalized p50 3.00s, p80 3.00s, p99 3.00s") {
		t.Fatalf("edited audit message missing percentiles: %s", got)
	}
	monitor.Off()
	if got := r.streamLine(run); got != "" {
		t.Fatalf("delivery spoiler remained after switching off: %s", got)
	}
}
