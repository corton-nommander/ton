package main

import (
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/botstatus"
)

func (c *cockpit) receiveBotStatus(w http.ResponseWriter, r *http.Request) {
	if c.cfg.StatusSecret == "" {
		http.NotFound(w, r)
		return
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 4096))
	if err != nil {
		http.Error(w, "invalid report size", http.StatusBadRequest)
		return
	}
	timestamp := r.Header.Get("X-Status-Timestamp")
	t, err := strconv.ParseInt(timestamp, 10, 64)
	if err != nil || !botstatus.Verify(c.cfg.StatusSecret, timestamp, body, r.Header.Get("X-Status-Signature")) {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	now := time.Now()
	age := now.Sub(time.Unix(0, t))
	if age > 30*time.Second || age < -5*time.Second {
		http.Error(w, "expired report", http.StatusUnauthorized)
		return
	}
	var report botstatus.Report
	if err := json.Unmarshal(body, &report); err != nil || report.Network != c.cfg.Network || report.PoolTotal < 0 || report.PoolSize < 0 {
		http.Error(w, "invalid report", http.StatusBadRequest)
		return
	}
	c.state.mu.Lock()
	if t <= c.state.lastBotTimestamp {
		c.state.mu.Unlock()
		http.Error(w, "outdated report", http.StatusConflict)
		return
	}
	c.state.lastBotTimestamp = t
	c.state.data.Bot = Reading[botstatus.Report]{Data: report, UpdatedAt: now.UTC()}
	c.state.mu.Unlock()
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusNoContent)
}
