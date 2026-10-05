package botstatus

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"time"
)

// Report is a best-effort snapshot of the runs known to one bot process.
// Static contract runs are only tracked until the bot restarts.
type Report struct {
	Network   string     `json:"network"`
	PoolTotal int        `json:"pool_total"`
	PoolSize  int        `json:"pool_size"`
	Pool      *PoolRun   `json:"pool,omitempty"`
	Static    *StaticRun `json:"static,omitempty"`
	Shards    *StaticRun `json:"shards,omitempty"`
}

type PoolRun struct {
	Mode      string    `json:"mode"`
	TargetRPS int       `json:"target_rps"`
	StartedAt time.Time `json:"started_at"`
	Sent      uint64    `json:"sent"`
	Failed    uint64    `json:"failed"`
	Skipped   uint64    `json:"skipped"`
	Delivery  *Delivery `json:"delivery,omitempty"`
}

// Delivery measures sampled transfers only (at most one observation per second),
// not the throughput of every message sent by the load generator.
type Delivery struct {
	Enabled             bool    `json:"enabled"`
	Ready               bool    `json:"ready"`
	Confirmed           uint64  `json:"confirmed"`
	Finalized           uint64  `json:"finalized"`
	Pending             uint64  `json:"pending"`
	NotFound            uint64  `json:"not_found"`
	FinalizedNotFound   uint64  `json:"finalized_not_found"`
	Interrupted         uint64  `json:"interrupted"`
	SendFailed          uint64  `json:"send_failed"`
	NoSlots             uint64  `json:"no_slots"`
	ConfirmedPerSecond  float64 `json:"confirmed_per_second"`
	FinalizedPerSecond  float64 `json:"finalized_per_second"`
	P50Seconds          float64 `json:"p50_seconds"`
	P95Seconds          float64 `json:"p95_seconds"`
	FinalizedP50Seconds float64 `json:"finalized_p50_seconds"`
	FinalizedP95Seconds float64 `json:"finalized_p95_seconds"`
}

type StaticRun struct {
	TPS       int       `json:"tps_per_shard"`
	Shards    int       `json:"shards"`
	StartedAt time.Time `json:"started_at"`
}

// Signature authenticates the timestamp and exact JSON body over plain HTTP.
func Signature(secret, timestamp string, body []byte) string {
	mac := hmac.New(sha256.New, []byte(secret))
	_, _ = mac.Write([]byte(timestamp))
	_, _ = mac.Write([]byte("\n"))
	_, _ = mac.Write(body)
	return hex.EncodeToString(mac.Sum(nil))
}

func Verify(secret, timestamp string, body []byte, signature string) bool {
	got, err := hex.DecodeString(signature)
	if err != nil {
		return false
	}
	want, _ := hex.DecodeString(Signature(secret, timestamp, body))
	return hmac.Equal(got, want)
}
