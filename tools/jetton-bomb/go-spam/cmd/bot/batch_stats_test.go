package main

import (
	"strings"
	"testing"
)

func TestBatchRPCBreakdownCountsExternalsNotGaslessRequests(t *testing.T) {
	cur := Snap{Sent: 100, Seqno1: 100, BatchSend: 12, BatchRetry: 2, Batches: 12}
	html := formatRequestsBreakdownHTML(cur, Snap{}, 2)
	if !strings.Contains(html, "requests <b>57/s</b>") {
		t.Fatalf("incorrect actual backend RPS: %s", html)
	}
	if !strings.Contains(html, "BatchSend") || !strings.Contains(html, "BatchRetry") {
		t.Fatal("missing batch RPC counters")
	}
}
