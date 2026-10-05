package main

import (
	"context"
	"embed"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"log"
	"net/http"
	"net/url"
	"strconv"
	"sync"
	"time"
)

//go:embed static/*
var assets embed.FS

type cockpit struct {
	cfg       config
	http      *http.Client
	state     state
	cacheMu   sync.Mutex
	charts    map[string]cachedChart
	fuelTrend []fuelPoint
}

type cachedChart struct {
	value   []byte
	expires time.Time
}

func newCockpit(cfg config) *cockpit {
	return &cockpit{
		cfg:    cfg,
		http:   &http.Client{Timeout: 12 * time.Second},
		state:  state{data: snapshot{Network: cfg.Network}},
		charts: make(map[string]cachedChart),
	}
}

func (c *cockpit) start(ctx context.Context) {
	c.poll(ctx, time.Second, "tpsmeter", func(ctx context.Context) {
		v, err := c.getMeter(ctx)
		updateReading(&c.state.mu, &c.state.data.Meter, v, err)
	})
	c.poll(ctx, 2*time.Second, "blocks", func(ctx context.Context) {
		v, err := c.getChain(ctx)
		updateReading(&c.state.mu, &c.state.data.Chain, v, err)
	})
	c.poll(ctx, 15*time.Second, "prometheus", func(ctx context.Context) {
		if c.cfg.Network != "mainnet" {
			return
		}
		v, err := c.getMainnetLS(ctx)
		updateReading(&c.state.mu, &c.state.data.LS, v, err)
	})
	c.poll(ctx, time.Minute, "testnet logs", func(ctx context.Context) {
		if c.cfg.Network != "testnet" {
			return
		}
		v, err := c.getTestnetLS(ctx)
		updateReading(&c.state.mu, &c.state.data.LS, v, err)
	})
	c.poll(ctx, 30*time.Second, "service health", func(ctx context.Context) {
		v, err := c.getHealth(ctx)
		updateReading(&c.state.mu, &c.state.data.Health, v, err)
	})
	c.poll(ctx, time.Minute, "pool fuel", func(ctx context.Context) {
		v, err := c.getFuel(ctx)
		updateReading(&c.state.mu, &c.state.data.Fuel, v, err)
	})
	c.poll(ctx, time.Minute, "session stats", func(ctx context.Context) {
		v, err := c.getPressure(ctx)
		updateReading(&c.state.mu, &c.state.data.Pressure, v, err)
	})
}

func (c *cockpit) poll(ctx context.Context, interval time.Duration, name string, fn func(context.Context)) {
	go func() {
		fn(ctx)
		tick := time.NewTicker(interval)
		defer tick.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-tick.C:
				fn(ctx)
			}
		}
	}()
}

func (c *cockpit) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/snapshot", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(c.state.snapshot())
	})
	mux.HandleFunc("POST /internal/bot-status", c.receiveBotStatus)
	mux.HandleFunc("GET /api/history", c.history)
	mux.HandleFunc("GET /api/health", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]string{"status": "ok", "network": c.cfg.Network})
	})
	static, err := fs.Sub(assets, "static")
	if err != nil {
		panic(err)
	}
	mux.Handle("GET /", http.FileServer(http.FS(static)))
	return mux
}

func (c *cockpit) history(w http.ResponseWriter, r *http.Request) {
	spans := map[string]time.Duration{
		"15m": 15 * time.Minute,
		"1h":  time.Hour,
		"6h":  6 * time.Hour,
		"24h": 24 * time.Hour,
	}
	span := r.URL.Query().Get("span")
	if span == "" {
		span = "1h"
	}
	duration, ok := spans[span]
	if !ok {
		http.Error(w, "unsupported time range", http.StatusBadRequest)
		return
	}
	c.cacheMu.Lock()
	cached, ok := c.charts[span]
	c.cacheMu.Unlock()
	if ok && time.Now().Before(cached.expires) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(cached.value)
		return
	}
	current := c.state.snapshot().Meter.Data
	end := time.Now().UTC().Truncate(time.Second)
	if current.CoverageEnd != nil && current.CoverageEnd.Before(end) {
		end = *current.CoverageEnd
	}
	step := int(duration.Seconds() / 540)
	if step < 1 {
		step = 1
	}
	query := url.Values{
		"start":       {end.Add(-duration).Format(time.RFC3339)},
		"end":         {end.Format(time.RFC3339)},
		"window_size": {"60"},
		"step":        {strconv.Itoa(step)},
	}
	req, err := http.NewRequestWithContext(r.Context(), http.MethodGet, cleanURL(c.cfg.MeterURL)+"/v1/series?"+query.Encode(), nil)
	if err != nil {
		http.Error(w, "invalid history request", http.StatusInternalServerError)
		return
	}
	res, err := c.http.Do(req)
	if err != nil {
		http.Error(w, "tpsmeter unavailable", http.StatusBadGateway)
		return
	}
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		http.Error(w, fmt.Sprintf("tpsmeter HTTP %d", res.StatusCode), http.StatusBadGateway)
		return
	}
	body, err := io.ReadAll(io.LimitReader(res.Body, 2<<20))
	if err != nil || !json.Valid(body) {
		http.Error(w, "invalid tpsmeter history", http.StatusBadGateway)
		return
	}
	c.cacheMu.Lock()
	c.charts[span] = cachedChart{value: body, expires: time.Now().Add(15 * time.Second)}
	c.cacheMu.Unlock()
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write(body)
}

func (c *cockpit) get(ctx context.Context, addr string, key bool, target any) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, addr, nil)
	if err != nil {
		return err
	}
	if key && c.cfg.APIKey != "" {
		req.Header.Set("X-API-Key", c.cfg.APIKey)
	}
	res, err := c.http.Do(req)
	if err != nil {
		return err
	}
	return decodeJSON(res, target)
}

func logFailure(name string, err error) {
	if err != nil {
		log.Printf("%s: %v", name, err)
	}
}
