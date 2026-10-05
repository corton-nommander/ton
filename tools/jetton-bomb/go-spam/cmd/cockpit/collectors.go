package main

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"net/url"
	"os/exec"
	"sort"
	"strconv"
	"strings"
	"time"
)

func (c *cockpit) getMeter(ctx context.Context) (Meter, error) {
	var raw struct {
		Windows        []Window   `json:"windows"`
		Shards         int        `json:"shards"`
		ForwardSynced  bool       `json:"forward_synced"`
		CoverageValid  bool       `json:"coverage_valid"`
		Rollback       bool       `json:"rollback_detected"`
		LastMismatchAt *time.Time `json:"last_mismatch_at"`
		Pending        bool       `json:"pending_batch"`
		SourceVerified bool       `json:"source_verified"`
		LastError      string     `json:"last_error"`
		Store          struct {
			CoverageEnd *time.Time `json:"coverage_end"`
		} `json:"store"`
	}
	if err := c.get(ctx, cleanURL(c.cfg.MeterURL)+"/v1/status", false, &raw); err != nil {
		return Meter{}, err
	}
	recentMismatch := false
	if raw.LastMismatchAt != nil {
		age := time.Since(*raw.LastMismatchAt)
		recentMismatch = age >= 0 && age < time.Minute
	}
	return Meter{
		Windows: raw.Windows, Shards: raw.Shards, CoverageEnd: raw.Store.CoverageEnd,
		ForwardSynced: raw.ForwardSynced, CoverageValid: raw.CoverageValid,
		Rollback: raw.Rollback, RecentMismatch: recentMismatch, Pending: raw.Pending, SourceVerified: raw.SourceVerified,
		LastError: raw.LastError,
	}, nil
}

func (c *cockpit) getChain(ctx context.Context) (Chain, error) {
	var head Chain
	if err := c.get(ctx, cleanURL(c.cfg.MeterURL)+"/v1/head", false, &head); err != nil {
		return Chain{}, err
	}
	if head.MC.Seqno == 0 || head.MC.GenUtime <= 0 {
		return Chain{}, fmt.Errorf("tpsmeter LS head returned no masterchain block")
	}
	if len(head.Shards) == 0 {
		return Chain{}, fmt.Errorf("tpsmeter LS head returned no shard tips")
	}
	for _, tip := range head.Shards {
		if tip.Workchain != 0 || tip.Seqno == 0 || tip.GenUtime <= 0 {
			return Chain{}, fmt.Errorf("tpsmeter LS head returned an incomplete shard tip")
		}
	}
	if head.Source == "" {
		head.Source = "tpsmeter live block headers"
	}
	return head, nil
}

type promVector struct {
	Status string `json:"status"`
	Data   struct {
		Result []struct {
			Metric struct {
				Instance string `json:"instance"`
			} `json:"metric"`
			Value []json.RawMessage `json:"value"`
		} `json:"result"`
	} `json:"data"`
}

func (c *cockpit) promQuery(ctx context.Context, query string) (map[string]float64, error) {
	var result promVector
	endpoint := cleanURL(c.cfg.Prometheus) + "/api/v1/query?" + url.Values{"query": {query}}.Encode()
	if err := c.get(ctx, endpoint, false, &result); err != nil {
		return nil, err
	}
	if result.Status != "success" {
		return nil, fmt.Errorf("prometheus query failed")
	}
	out := make(map[string]float64)
	for _, row := range result.Data.Result {
		if len(row.Value) < 2 {
			continue
		}
		var text string
		if err := json.Unmarshal(row.Value[1], &text); err != nil {
			continue
		}
		value, err := strconv.ParseFloat(text, 64)
		if err == nil && !math.IsNaN(value) && !math.IsInf(value, 0) {
			out[row.Metric.Instance] = value
		}
	}
	return out, nil
}

func (c *cockpit) getMainnetLS(ctx context.Context) (Fleet, error) {
	const selector = `{job="toncenter-mainnet-ls"}`
	up, err := c.promQuery(ctx, "up"+selector)
	if err != nil {
		return Fleet{}, err
	}
	age, err := c.promQuery(ctx, "ton_masterchain_block_age_seconds"+selector)
	if err != nil {
		return Fleet{}, err
	}
	seqno, err := c.promQuery(ctx, "ton_masterchain_seqno"+selector)
	if err != nil {
		return Fleet{}, err
	}
	shardGap, err := c.promQuery(ctx, "ton_masterchain_seqno"+selector+" - ton_shardclient_seqno"+selector)
	if err != nil {
		return Fleet{}, err
	}
	if len(up) == 0 {
		return Fleet{}, fmt.Errorf("no mainnet liteserver targets in Prometheus")
	}
	out := Fleet{Source: "Prometheus / toncenter-mainnet-ls", Nodes: make([]Node, 0, len(up))}
	for host, value := range up {
		node := Node{Name: strings.Split(host, ":")[0], Up: value == 1, State: "unknown"}
		if !node.Up {
			node.State = "down"
		} else if v, ok := age[host]; ok {
			node.AgeSeconds = &v
			node.State = "ok"
			if v > 15 {
				node.State = "lagging"
			}
		}
		if v, ok := seqno[host]; ok {
			node.MCSeqno = &v
		}
		if v, ok := shardGap[host]; ok {
			node.ShardGap = &v
			if v > 5 && node.State == "ok" {
				node.State = "lagging"
			}
		}
		if node.State == "ok" {
			out.Healthy++
		}
		out.Nodes = append(out.Nodes, node)
	}
	sort.Slice(out.Nodes, func(i, j int) bool { return out.Nodes[i].Name < out.Nodes[j].Name })
	out.Total = len(out.Nodes)
	return out, nil
}

func (c *cockpit) getTestnetLS(ctx context.Context) (Fleet, error) {
	// The existing log server handles clock-skew correction and 60-second STATUS
	// reports. Avoid scanning large daily logs independently in the dashboard.
	const script = `import sys,json; sys.path.insert(0,sys.argv[1]); from tonlogs.tools_smart import sync_status; d=sync_status("testnet",last="15m"); print(json.dumps(d))`
	limited, cancel := context.WithTimeout(ctx, 35*time.Second)
	defer cancel()
	cmd := exec.CommandContext(limited, c.cfg.LogsPython, "-c", script, c.cfg.LogsModule)
	raw, err := cmd.Output()
	if err != nil {
		return Fleet{}, fmt.Errorf("sync_status: %w", err)
	}
	var data struct {
		Hosts map[string]struct {
			Latest struct {
				MC    float64 `json:"masterchain_block_ago_ms"`
				Shard float64 `json:"shard_client_ago_ms"`
			} `json:"latest"`
			LastReport string   `json:"last_report_utc"`
			Stale      float64  `json:"seconds_since_last_report"`
			Warnings   []string `json:"thresholds_crossed"`
		} `json:"hosts"`
		Missing map[string]any `json:"hosts_without_files_in_window"`
	}
	if err := json.Unmarshal(raw, &data); err != nil {
		return Fleet{}, fmt.Errorf("sync_status JSON: %w", err)
	}
	out := Fleet{Nodes: []Node{}, Source: "Validator STATUS logs / tonlogs.sync_status"}
	for host, row := range data.Hosts {
		if !strings.Contains(host, "ls") {
			continue
		}
		mc, shard := row.Latest.MC/1000, row.Latest.Shard/1000
		state := "ok"
		if row.Stale > 180 || row.LastReport == "" {
			state = "down"
		} else if mc > 15 || shard > 15 || len(row.Warnings) > 0 {
			state = "lagging"
		}
		if state == "ok" {
			out.Healthy++
		}
		out.Nodes = append(out.Nodes, Node{
			Name: host, AgeSeconds: &mc, ShardSeconds: &shard,
			ReportedAt: row.LastReport, Up: row.Stale <= 180, State: state,
		})
	}
	for host := range data.Missing {
		if strings.Contains(host, "ls") {
			out.Nodes = append(out.Nodes, Node{Name: host, State: "unknown"})
		}
	}
	sort.Slice(out.Nodes, func(i, j int) bool { return out.Nodes[i].Name < out.Nodes[j].Name })
	out.Total = len(out.Nodes)
	if out.Total == 0 {
		return Fleet{}, fmt.Errorf("no testnet liteserver logs found")
	}
	return out, nil
}

func (c *cockpit) getHealth(ctx context.Context) (Health, error) {
	base := cleanURL(c.cfg.HealthURL) + "/api/v1/"
	var status struct {
		LastSync string `json:"lastSyncAt"`
	}
	if err := c.get(ctx, base+"status", false, &status); err != nil {
		return Health{}, err
	}
	var groups []struct {
		Components []struct {
			ID      int    `json:"id"`
			Key     string `json:"key"`
			Name    string `json:"displayName"`
			Network string `json:"network"`
			Status  string `json:"status"`
		} `json:"components"`
	}
	if err := c.get(ctx, base+"components", false, &groups); err != nil {
		return Health{}, err
	}
	out := Health{LastSync: status.LastSync, Components: []HealthComponent{}}
	for _, group := range groups {
		for _, component := range group.Components {
			if component.Network != c.cfg.Network {
				continue
			}
			out.Components = append(out.Components, HealthComponent{Name: component.Name, Status: component.Status})
			if !strings.Contains(component.Key, "liveness-v2") && !strings.Contains(component.Key, "liveness-v3") && !strings.Contains(component.Key, "liveness-streaming") {
				continue
			}
			var uptime struct {
				Endpoints []struct {
					Key    string   `json:"endpointKey"`
					Status string   `json:"status"`
					Lag    *float64 `json:"currentLagSec"`
					P95    *float64 `json:"p95LagSec"`
				} `json:"endpoints"`
			}
			endpoint := fmt.Sprintf("%scomponents/%d/uptime?window=1h", base, component.ID)
			if err := c.get(ctx, endpoint, false, &uptime); err != nil {
				return Health{}, err
			}
			for _, item := range uptime.Endpoints {
				if strings.Contains(item.Key, "freshness") || strings.HasPrefix(item.Key, "SSE ") || strings.HasPrefix(item.Key, "WS ") {
					out.Components = append(out.Components, HealthComponent{Name: item.Key, Status: item.Status, LagSeconds: item.Lag, LagP95: item.P95})
				}
			}
		}
	}
	return out, nil
}

func (c *cockpit) getPressure(ctx context.Context) (Pressure, error) {
	end := time.Now().UTC().Truncate(time.Second)
	query := url.Values{
		"start":       {end.Add(-10 * time.Minute).Format(time.RFC3339)},
		"end":         {end.Format(time.RFC3339)},
		"wc":          {"0"},
		"mode":        {"avg"},
		"window_size": {"60"},
		"stats":       {"BLOCK_msg_queue_size,BLOCK_collate_ext_msgs_rejected,BLOCK_collate_overload_timeout,BLOCK_collate_work_time_real_prelim_storage_stat"},
	}
	var rows []struct {
		Timestamp string  `json:"timestamp"`
		Queue     float64 `json:"t1"`
		Rejected  float64 `json:"t2"`
		Timeout   float64 `json:"t3"`
		Collation float64 `json:"t4"`
	}
	if err := c.get(ctx, cleanURL(c.cfg.StatsURL)+"/api/stats?"+query.Encode(), false, &rows); err != nil {
		return Pressure{}, err
	}
	if len(rows) == 0 {
		return Pressure{}, fmt.Errorf("session stats has no recent samples")
	}
	last := rows[len(rows)-1]
	observedAt, err := time.Parse(time.RFC3339, last.Timestamp)
	if err != nil {
		return Pressure{}, fmt.Errorf("session stats has no recent block measurements")
	}
	return Pressure{ObservedAt: observedAt.Format(time.RFC3339), QueueSize: last.Queue, Rejected: last.Rejected, TimeoutLoad: last.Timeout, CollationTime: last.Collation}, nil
}
