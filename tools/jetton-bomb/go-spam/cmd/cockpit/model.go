package main

import (
	"sync"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/botstatus"
)

// Reading keeps the measurement and its collection time together. Errors do
// not erase the last good measurement; consumers can distinguish stale data.
type Reading[T any] struct {
	Data      T         `json:"data"`
	UpdatedAt time.Time `json:"updated_at,omitempty"`
	Error     string    `json:"error,omitempty"`
}

type Window struct {
	WindowSize int     `json:"window_size"`
	TPS        float64 `json:"tps"`
	Filling    bool    `json:"filling"`
	BlockRate  float64 `json:"block_rate"`
}

type Meter struct {
	Windows        []Window   `json:"windows"`
	Shards         int        `json:"shards"`
	CoverageEnd    *time.Time `json:"coverage_end"`
	ForwardSynced  bool       `json:"forward_synced"`
	CoverageValid  bool       `json:"coverage_valid"`
	Rollback       bool       `json:"rollback_detected"`
	RecentMismatch bool       `json:"recent_mismatch"`
	Pending        bool       `json:"pending_batch"`
	SourceVerified bool       `json:"source_verified"`
	LastError      string     `json:"last_error,omitempty"`
}

type BlockTip struct {
	Workchain int    `json:"workchain"`
	Shard     string `json:"shard"`
	Seqno     int64  `json:"seqno"`
	GenUtime  int64  `json:"gen_utime,string"`
}

type Chain struct {
	MC     BlockTip   `json:"mc"`
	Shards []BlockTip `json:"shards"`
	Source string     `json:"source"`
}

type Node struct {
	Name         string   `json:"name"`
	AgeSeconds   *float64 `json:"age_seconds,omitempty"`
	ShardSeconds *float64 `json:"shard_seconds,omitempty"`
	ShardGap     *float64 `json:"shard_gap_blocks,omitempty"`
	MCSeqno      *float64 `json:"mc_seqno,omitempty"`
	Up           bool     `json:"up"`
	ReportedAt   string   `json:"reported_at,omitempty"`
	State        string   `json:"state"`
}

type Fleet struct {
	Nodes   []Node `json:"nodes"`
	Healthy int    `json:"healthy"`
	Total   int    `json:"total"`
	Source  string `json:"source"`
}

type HealthComponent struct {
	Name       string   `json:"name"`
	Status     string   `json:"status"`
	LagSeconds *float64 `json:"lag_seconds,omitempty"`
	LagP95     *float64 `json:"lag_p95,omitempty"`
}

type Health struct {
	Components []HealthComponent `json:"components"`
	LastSync   string            `json:"last_sync,omitempty"`
}

type Fuel struct {
	Sampled        int      `json:"sampled"`
	Count          int      `json:"count"`
	Scope          string   `json:"scope"`
	Average        float64  `json:"average"`
	P10            float64  `json:"p10"`
	JettonAverage  float64  `json:"jetton_average"`
	BelowThreshold int      `json:"below_threshold"`
	Threshold      float64  `json:"threshold"`
	Initial        float64  `json:"initial"`
	RemainingPct   float64  `json:"remaining_pct"`
	Missing        int      `json:"missing"`
	MinutesLeft    *float64 `json:"minutes_left,omitempty"`
	Source         string   `json:"source"`
}

type Pressure struct {
	ObservedAt    string  `json:"observed_at"`
	QueueSize     float64 `json:"queue_size"`
	Rejected      float64 `json:"rejected"`
	TimeoutLoad   float64 `json:"timeout_load"`
	CollationTime float64 `json:"collation_time"`
}

type snapshot struct {
	Network  string                    `json:"network"`
	Now      time.Time                 `json:"now"`
	Meter    Reading[Meter]            `json:"meter"`
	Chain    Reading[Chain]            `json:"chain"`
	LS       Reading[Fleet]            `json:"ls"`
	Health   Reading[Health]           `json:"health"`
	Fuel     Reading[Fuel]             `json:"fuel"`
	Pressure Reading[Pressure]         `json:"pressure"`
	Bot      Reading[botstatus.Report] `json:"bot"`
	BotFresh bool                      `json:"bot_fresh"`
}

type state struct {
	mu               sync.RWMutex
	data             snapshot
	lastBotTimestamp int64
}

func updateReading[T any](mu *sync.RWMutex, dst *Reading[T], value T, err error) {
	mu.Lock()
	defer mu.Unlock()
	if err != nil {
		dst.Error = err.Error()
		return
	}
	dst.Data = value
	dst.UpdatedAt = time.Now().UTC()
	dst.Error = ""
}

func (s *state) snapshot() snapshot {
	s.mu.RLock()
	defer s.mu.RUnlock()
	v := s.data
	v.Now = time.Now().UTC()
	v.BotFresh = !v.Bot.UpdatedAt.IsZero() && v.Now.Sub(v.Bot.UpdatedAt) >= 0 && v.Now.Sub(v.Bot.UpdatedAt) < 20*time.Second
	return v
}
