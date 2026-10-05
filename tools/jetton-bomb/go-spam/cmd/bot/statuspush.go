package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/botstatus"
)

type deliverySample struct {
	runID                int64
	at                   time.Time
	confirmed, finalized uint64
}

func sampledDeliveryRates(previous *deliverySample, runID int64, confirmed, finalized uint64, now time.Time) (float64, float64) {
	var confirmedRate, finalizedRate float64
	if previous.runID == runID && now.After(previous.at) && confirmed >= previous.confirmed && finalized >= previous.finalized {
		seconds := now.Sub(previous.at).Seconds()
		confirmedRate = float64(confirmed-previous.confirmed) / seconds
		finalizedRate = float64(finalized-previous.finalized) / seconds
	}
	*previous = deliverySample{runID: runID, at: now, confirmed: confirmed, finalized: finalized}
	return confirmedRate, finalizedRate
}

func (r *Runtime) statusReport(network string, previous *deliverySample) botstatus.Report {
	status := botstatus.Report{Network: network, PoolTotal: r.poolTotal, PoolSize: r.SubPoolSize()}
	if run := r.poolRun.Load(); run != nil {
		status.Pool = &botstatus.PoolRun{
			Mode: run.kind, TargetRPS: run.freq, StartedAt: run.startedAt,
			Sent: run.stats.Sent.Load(), Failed: run.stats.Failed.Load(), Skipped: run.stats.Skipped.Load(),
		}
		if r.stream != nil && r.streamMemory != nil {
			runID := run.startedAt.UnixNano()
			observed := r.streamMemory.Snapshot(runID)
			confirmedRate, finalizedRate := sampledDeliveryRates(previous, runID, observed.Confirmed, observed.Finalized, time.Now())
			status.Pool.Delivery = &botstatus.Delivery{
				Enabled: r.stream.Enabled(), Ready: r.stream.Ready(),
				Confirmed: observed.Confirmed, Finalized: observed.Finalized, Pending: uint64(r.stream.Pending(runID)),
				NotFound: observed.Missing, FinalizedNotFound: observed.FinalizedMissing,
				Interrupted: observed.Interrupted, SendFailed: observed.SendFailed, NoSlots: observed.NoSlot,
				ConfirmedPerSecond: confirmedRate, FinalizedPerSecond: finalizedRate,
				P50Seconds: observed.P50.Seconds(), P95Seconds: observed.P95.Seconds(),
				FinalizedP50Seconds: observed.FinalizedP50.Seconds(), FinalizedP95Seconds: observed.FinalizedP95.Seconds(),
			}
		}
	}
	if run := r.staticRun.Load(); run != nil {
		status.Static = &botstatus.StaticRun{TPS: run.tps, Shards: run.shards, StartedAt: run.startedAt}
	}
	if run := r.shardsRun.Load(); run != nil {
		status.Shards = &botstatus.StaticRun{TPS: run.tps, Shards: run.shards, StartedAt: run.startedAt}
	}
	return status
}

func (r *Runtime) sendStatusReport(ctx context.Context, client *http.Client, url, secret, network string, previous *deliverySample) {
	body, err := json.Marshal(r.statusReport(network, previous))
	if err != nil {
		return
	}
	timestamp := strconv.FormatInt(time.Now().UnixNano(), 10)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Status-Timestamp", timestamp)
	req.Header.Set("X-Status-Signature", botstatus.Signature(secret, timestamp, body))
	if resp, err := client.Do(req); err == nil {
		_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 1024))
		_ = resp.Body.Close()
	}
}

// Status reporting never blocks bot commands or the load loop. A failed POST
// is simply replaced by the next snapshot; the receiver marks missing data stale.
func (r *Runtime) reportStatus(ctx context.Context, url, secret, network string) {
	if url == "" || secret == "" || (network != "mainnet" && network != "testnet") {
		return
	}
	client := &http.Client{Timeout: 2 * time.Second}
	previous := &deliverySample{}
	r.sendStatusReport(ctx, client, url, secret, network, previous)
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			r.sendStatusReport(ctx, client, url, secret, network, previous)
		}
	}
}
