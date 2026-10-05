package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/botstatus"
	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
	"github.com/victor/jetton-bomb-go-spam/pkg/streaming"
)

func TestStatusReportPushesCurrentRuns(t *testing.T) {
	r := &Runtime{poolTotal: 5000}
	r.subPoolSize.Store(1000)
	stats := &pool.Stats{}
	stats.Sent.Store(42)
	r.poolRun.Store(&SpamRun{kind: "jetton", freq: 150, startedAt: time.Now().Add(-time.Minute), stats: stats})
	r.staticRun.Store(&StaticRun{tps: 10, shards: 1, startedAt: time.Now().Add(-time.Hour)})
	called := false
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		called = true
		body, err := io.ReadAll(req.Body)
		if err != nil || !botstatus.Verify("key", req.Header.Get("X-Status-Timestamp"), body, req.Header.Get("X-Status-Signature")) {
			t.Error("report was not authenticated")
		}
		var report botstatus.Report
		if err := json.Unmarshal(body, &report); err != nil {
			t.Fatal(err)
		}
		if report.Network != "mainnet" || report.PoolTotal != 5000 || report.PoolSize != 1000 ||
			report.Pool == nil || report.Pool.Mode != "jetton" || report.Pool.TargetRPS != 150 || report.Pool.Sent != 42 ||
			report.Static == nil || report.Static.TPS != 10 || report.Shards != nil {
			t.Errorf("incorrect active load: %+v", report)
		}
	}))
	defer server.Close()
	r.sendStatusReport(context.Background(), server.Client(), server.URL, "key", "mainnet", &deliverySample{})
	if !called {
		t.Fatal("no status request was sent")
	}
	r.poolRun.Store(nil)
	if r.statusReport("mainnet", &deliverySample{}).Pool != nil {
		t.Fatal("finished run remained active in status")
	}
}

func TestSampledDeliveryRatesPerRun(t *testing.T) {
	previous := &deliverySample{}
	start := time.Unix(100, 0)
	if got, final := sampledDeliveryRates(previous, 42, 2, 1, start); got != 0 || final != 0 {
		t.Fatalf("first report should warm up rates, got %.2f %.2f", got, final)
	}
	if got, final := sampledDeliveryRates(previous, 42, 7, 4, start.Add(5*time.Second)); got != 1 || final != 0.6 {
		t.Fatalf("wrong per-window delivery rates: %.2f %.2f", got, final)
	}
	if got, final := sampledDeliveryRates(previous, 43, 1, 1, start.Add(10*time.Second)); got != 0 || final != 0 {
		t.Fatalf("new run reused old rates: %.2f %.2f", got, final)
	}
}

func TestStatusReportIncludesStreamingOutcomes(t *testing.T) {
	started := time.Now().Add(-time.Minute)
	memory := streaming.NewMemory()
	monitor := streaming.New("", "", 1, memory, nil)
	defer monitor.Close()
	memory.Record(streaming.Result{Intent: streaming.Intent{RunID: started.UnixNano()}, Outcome: "confirmed", Latency: 500 * time.Millisecond})
	memory.Record(streaming.Result{Intent: streaming.Intent{RunID: started.UnixNano()}, Outcome: "finalized", Latency: 900 * time.Millisecond})
	r := &Runtime{poolTotal: 100, stream: monitor, streamMemory: memory}
	r.poolRun.Store(&SpamRun{kind: "jetton", freq: 10, startedAt: started, stats: &pool.Stats{}})
	status := r.statusReport("testnet", &deliverySample{})
	if status.Pool == nil || status.Pool.Delivery == nil || status.Pool.Delivery.Confirmed != 1 ||
		status.Pool.Delivery.Finalized != 1 || status.Pool.Delivery.P50Seconds != 0.5 ||
		status.Pool.Delivery.FinalizedP95Seconds != 0.9 {
		t.Fatalf("streaming outcomes missing from status: %+v", status.Pool)
	}
}

func TestStatusPushFailureDoesNotBlock(t *testing.T) {
	r := &Runtime{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		time.Sleep(100 * time.Millisecond)
	}))
	defer server.Close()
	start := time.Now()
	r.sendStatusReport(context.Background(), &http.Client{Timeout: 20 * time.Millisecond}, server.URL, "key", "testnet", &deliverySample{})
	if time.Since(start) > 80*time.Millisecond {
		t.Fatal("unavailable receiver blocked status sender beyond its timeout")
	}
}
