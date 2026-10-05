package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strconv"
	"testing"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/botstatus"
)

func TestBotStatusReceiverAuthenticatesAndRejectsReplay(t *testing.T) {
	c := newCockpit(config{Network: "testnet", StatusSecret: "secret"})
	handler := c.routes()
	report := botstatus.Report{Network: "testnet", PoolTotal: 1000, PoolSize: 500,
		Pool: &botstatus.PoolRun{Mode: "jetton", TargetRPS: 50,
			Delivery: &botstatus.Delivery{Confirmed: 5, Finalized: 3, ConfirmedPerSecond: 0.8, FinalizedPerSecond: 0.4}},
	}
	body, _ := json.Marshal(report)
	timestamp := strconv.FormatInt(time.Now().UnixNano(), 10)
	send := func(payload []byte, ts, signature string) int {
		t.Helper()
		req := httptest.NewRequest(http.MethodPost, "/internal/bot-status", bytes.NewReader(payload))
		req.Header.Set("X-Status-Timestamp", ts)
		req.Header.Set("X-Status-Signature", signature)
		res := httptest.NewRecorder()
		handler.ServeHTTP(res, req)
		return res.Code
	}
	signature := botstatus.Signature("secret", timestamp, body)
	if got := send(body, timestamp, "invalid"); got != http.StatusUnauthorized {
		t.Fatalf("invalid signature status = %d", got)
	}
	if got := send(body, timestamp, signature); got != http.StatusNoContent {
		t.Fatalf("valid report status = %d", got)
	}
	if got := send(body, timestamp, signature); got != http.StatusConflict {
		t.Fatalf("replayed report status = %d", got)
	}
	oldTimestamp := strconv.FormatInt(time.Now().Add(-time.Minute).UnixNano(), 10)
	if got := send(body, oldTimestamp, botstatus.Signature("secret", oldTimestamp, body)); got != http.StatusUnauthorized {
		t.Fatalf("expired report status = %d", got)
	}
	badNetwork := botstatus.Report{Network: "mainnet"}
	badBody, _ := json.Marshal(badNetwork)
	newTimestamp := strconv.FormatInt(time.Now().Add(time.Millisecond).UnixNano(), 10)
	if got := send(badBody, newTimestamp, botstatus.Signature("secret", newTimestamp, badBody)); got != http.StatusBadRequest {
		t.Fatalf("cross-network report status = %d", got)
	}
	snapshot := c.state.snapshot()
	if !snapshot.BotFresh || snapshot.Bot.UpdatedAt.IsZero() || snapshot.Bot.Data.Network != "testnet" || snapshot.Bot.Data.PoolSize != 500 ||
		snapshot.Bot.Data.Pool == nil || snapshot.Bot.Data.Pool.Delivery == nil || snapshot.Bot.Data.Pool.Delivery.ConfirmedPerSecond != 0.8 {
		t.Fatalf("receiver lost last authenticated report: %+v", snapshot.Bot)
	}
	c.state.mu.Lock()
	c.state.data.Bot.UpdatedAt = time.Now().Add(-time.Minute)
	c.state.mu.Unlock()
	if c.state.snapshot().BotFresh {
		t.Fatal("missing reports were treated as an idle, live bot")
	}
}
