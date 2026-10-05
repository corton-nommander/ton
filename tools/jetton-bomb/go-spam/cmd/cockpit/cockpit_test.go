package main

import (
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/jetton"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
	"github.com/xssnick/tonutils-go/address"
)

func TestSampleIDsExcludeDriverAndTerminal(t *testing.T) {
	pub := make([]byte, ed25519.PublicKeySize)
	ids := sampleIDs(pub, 5000, 20)
	if len(ids) != 20 {
		t.Fatalf("got %d sample ids", len(ids))
	}
	seen := map[uint32]bool{}
	for i, id := range ids {
		if id == 0 || id >= 5000 || seen[id] {
			t.Fatalf("invalid or repeated id %d", id)
		}
		seen[id] = true
		if lo, hi := 1+i*4999/20, 1+(i+1)*4999/20; int(id) < lo || int(id) >= hi {
			t.Fatalf("id %d is outside stratum [%d,%d)", id, lo, hi)
		}
	}
	if got := sampleIDs(pub, 5000, 20); fmt.Sprint(got) != fmt.Sprint(ids) {
		t.Fatalf("sample changed for identical pubkey: %v vs %v", got, ids)
	}
}

func TestFuelUsesOneBatchForBothWalletKinds(t *testing.T) {
	pub := make([]byte, ed25519.PublicKeySize)
	for i := range pub {
		pub[i] = byte(i + 1)
	}
	minter := address.MustParseRawAddr("0:0000000000000000000000000000000000000000000000000000000000000abc")
	jwCode, code := walletspam.LoadJettonWalletCode(), walletspam.LoadCode(walletspam.CodeHex)
	config := poolConfig{
		PublicKey: hex.EncodeToString(pub), Minter: minter.String(), Count: 5000,
		InitialBalance: 0.2, JettonInitial: 0.01, SendThreshold: 0.06,
		ExpectedWallet0: walletspam.New(0, pub, nil, minter, jwCode, code).Address().StringRaw(),
	}
	path := filepath.Join(t.TempDir(), "pool.json")
	raw, _ := json.Marshal(config)
	if err := os.WriteFile(path, raw, 0600); err != nil {
		t.Fatal(err)
	}
	wallets := map[string]string{}
	for i, id := range sampleIDs(pub, config.Count, 20) {
		owner := walletspam.New(id, pub, nil, minter, jwCode, code).Address()
		balance := "200000000"
		if i < 2 {
			balance = "50000000"
		}
		wallets[owner.StringRaw()] = balance
		wallets[jetton.WalletAddress(owner, minter, jwCode).StringRaw()] = "10000000"
	}
	requests := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests++
		if r.URL.Path != "/accountStates" || r.URL.Query().Get("include_boc") != "false" || len(r.URL.Query()["address"]) != 40 {
			t.Errorf("unexpected batch request %s", r.URL.String())
		}
		var rows []map[string]string
		for _, input := range r.URL.Query()["address"] {
			balance, ok := wallets[input]
			if !ok {
				t.Errorf("unknown sampled address %s", input)
				continue
			}
			rows = append(rows, map[string]string{"address": strings.ToUpper(input), "balance": balance})
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"accounts": rows})
	}))
	defer server.Close()
	c := newCockpit(configForTest(server.URL))
	c.cfg.PoolFile = path
	got, err := c.getFuel(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if requests != 1 || got.Sampled != 20 || got.Missing != 0 || got.BelowThreshold != 2 {
		t.Fatalf("batch count=%d fuel=%+v", requests, got)
	}
	if got.Average < 0.184 || got.Average > 0.186 || math.Abs(got.JettonAverage-0.01) > 1e-9 || got.P10 != 0.05 {
		t.Fatalf("unexpected balances %+v", got)
	}
}

func TestBlockTipsPreserveEachShardTime(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		if r.URL.Path != "/v1/head" {
			http.NotFound(w, r)
			return
		}
		_, _ = w.Write([]byte(`{"mc":{"workchain":-1,"shard":"8000000000000000","seqno":10,"gen_utime":"100"},"shards":[{"workchain":0,"shard":"4000000000000000","seqno":20,"gen_utime":"99"},{"workchain":0,"shard":"C000000000000000","seqno":17,"gen_utime":"61"}],"source":"tpsmeter liteserver head"}`))
	}))
	defer server.Close()
	c := newCockpit(configForTest(server.URL))
	result, err := c.getChain(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if result.Source != "tpsmeter liteserver head" || result.MC.GenUtime != 100 || len(result.Shards) != 2 || result.Shards[0].GenUtime != 99 || result.Shards[1].GenUtime != 61 {
		t.Fatalf("per-shard block times lost: %+v", result)
	}
}

func TestBlockTipsRejectIncompleteLSHeadWithoutComparingLaggingMeter(t *testing.T) {
	var complete atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		if r.URL.Path != "/v1/head" {
			http.NotFound(w, r)
			return
		}
		if complete.Load() {
			_, _ = w.Write([]byte(`{"mc":{"workchain":-1,"shard":"8000000000000000","seqno":10,"gen_utime":"100"},"shards":[{"workchain":0,"shard":"2000000000000000","seqno":20,"gen_utime":"99"},{"workchain":0,"shard":"A000000000000000","seqno":21,"gen_utime":"99"}]}`))
		} else {
			_, _ = w.Write([]byte(`{"mc":{"workchain":-1,"shard":"8000000000000000","seqno":10,"gen_utime":"100"},"shards":[{"workchain":0,"shard":"2000000000000000","seqno":20,"gen_utime":"0"}]}`))
		}
	}))
	defer server.Close()
	c := newCockpit(configForTest(server.URL))
	head := time.Now().UTC()
	updateReading(&c.state.mu, &c.state.data.Meter, Meter{Shards: 4, CoverageEnd: &head, ForwardSynced: true, SourceVerified: true}, nil)
	if _, err := c.getChain(context.Background()); err == nil || !strings.Contains(err.Error(), "incomplete shard tip") {
		t.Fatalf("incomplete LS shard state was accepted: %v", err)
	}
	complete.Store(true)
	got, err := c.getChain(context.Background())
	if err != nil || len(got.Shards) != 2 {
		t.Fatalf("live LS head was incorrectly compared to an older meter checkpoint: %+v, %v", got, err)
	}
}

func TestStatusRetainsLastGoodMeasurementOnError(t *testing.T) {
	var state state
	updateReading(&state.mu, &state.data.Meter, Meter{Shards: 2}, nil)
	before := state.data.Meter.UpdatedAt
	time.Sleep(time.Millisecond)
	updateReading(&state.mu, &state.data.Meter, Meter{}, fmt.Errorf("liteserver unavailable"))
	if state.data.Meter.Data.Shards != 2 || !state.data.Meter.UpdatedAt.Equal(before) || state.data.Meter.Error == "" {
		t.Fatalf("the last good measurement should remain visibly stale: %+v", state.data.Meter)
	}
}

func TestMeterWarnsOnlyForRecentMismatch(t *testing.T) {
	for _, test := range []struct {
		name    string
		age     time.Duration
		missing bool
		want    bool
	}{
		{name: "recent", age: 30 * time.Second, want: true},
		{name: "old", age: 2 * time.Minute},
		{name: "future", age: -5 * time.Minute},
		{name: "legacy store", missing: true},
	} {
		t.Run(test.name, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path != "/v1/status" {
					t.Errorf("unexpected request %s", r.URL.Path)
				}
				status := map[string]any{
					"rollback_detected": true,
					"coverage_valid":    false,
				}
				if !test.missing {
					status["last_mismatch_at"] = time.Now().Add(-test.age).UTC()
				}
				_ = json.NewEncoder(w).Encode(status)
			}))
			defer server.Close()
			cfg := configForTest(server.URL)
			cfg.MeterURL = server.URL
			meter, err := newCockpit(cfg).getMeter(context.Background())
			if err != nil || meter.RecentMismatch != test.want {
				t.Fatalf("recent=%t, want %t: %v", meter.RecentMismatch, test.want, err)
			}
		})
	}
}

func configForTest(url string) config {
	return config{Network: "mainnet", V3URL: url, MeterURL: url}
}
