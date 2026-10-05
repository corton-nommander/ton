package main

import (
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"math/big"
	"net/url"
	"os"
	"sort"
	"strings"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/jetton"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
	"github.com/xssnick/tonutils-go/address"
)

// poolConfig contains public address-derivation inputs only. Acton deploys
// [0,count]; the first and terminal wallets are excluded from the sample.
type poolConfig struct {
	PublicKey       string  `json:"public_key"`
	Minter          string  `json:"minter"`
	Count           int     `json:"count"`
	InitialBalance  float64 `json:"initial_balance"`
	JettonInitial   float64 `json:"jetton_initial_balance"`
	SendThreshold   float64 `json:"send_threshold"`
	ExpectedWallet0 string  `json:"expected_wallet_0,omitempty"`
}

type fuelPoint struct {
	at  time.Time
	avg float64
}

func readPoolConfig(path string) (poolConfig, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return poolConfig{}, err
	}
	var pool poolConfig
	if err := json.Unmarshal(data, &pool); err != nil {
		return poolConfig{}, err
	}
	pub, err := hex.DecodeString(pool.PublicKey)
	if err != nil || len(pub) != ed25519.PublicKeySize {
		return poolConfig{}, fmt.Errorf("public_key must be a 32-byte hex Ed25519 public key")
	}
	if _, err := address.ParseAddr(pool.Minter); err != nil {
		if _, rawErr := address.ParseRawAddr(pool.Minter); rawErr != nil {
			return poolConfig{}, fmt.Errorf("invalid minter: %w", err)
		}
	}
	if pool.Count < 3 || pool.InitialBalance <= 0 || pool.SendThreshold < 0 || pool.SendThreshold >= pool.InitialBalance {
		return poolConfig{}, fmt.Errorf("count must be >=3 and initial_balance must exceed send_threshold")
	}
	return pool, nil
}

func parseTonAddress(value string) (*address.Address, error) {
	if strings.Contains(value, ":") {
		return address.ParseRawAddr(value)
	}
	return address.ParseAddr(value)
}

// sampleIDs chooses one deterministic pseudo-random wallet per evenly sized
// interior stratum. This avoids endpoint funding outliers and keeps the same
// wallets from one reading to the next, making consumption trends meaningful.
func sampleIDs(pub []byte, count, size int) []uint32 {
	available := count - 1 // ids [1,count-1]; exclude both [0,count]
	if available <= 0 || size <= 0 {
		return nil
	}
	if size > available {
		size = available
	}
	ids := make([]uint32, size)
	for i := range ids {
		start := i * available / size
		end := (i + 1) * available / size
		seed := make([]byte, len(pub)+4)
		copy(seed, pub)
		binary.BigEndian.PutUint32(seed[len(pub):], uint32(i))
		hash := sha256.Sum256(seed)
		ids[i] = uint32(1 + start + int(binary.BigEndian.Uint64(hash[:8])%uint64(end-start)))
	}
	return ids
}

func nanoToGRAM(raw string) (float64, error) {
	n, ok := new(big.Int).SetString(raw, 10)
	if !ok || n.Sign() < 0 {
		return 0, fmt.Errorf("invalid account balance")
	}
	v, _ := new(big.Rat).SetFrac(n, big.NewInt(1_000_000_000)).Float64()
	return v, nil
}

func (c *cockpit) getFuel(ctx context.Context) (Fuel, error) {
	if c.cfg.PoolFile == "" {
		return Fuel{}, fmt.Errorf("public pool configuration is not installed")
	}
	pool, err := readPoolConfig(c.cfg.PoolFile)
	if err != nil {
		return Fuel{}, err
	}
	pub, _ := hex.DecodeString(pool.PublicKey)
	minter, _ := parseTonAddress(pool.Minter)
	code := walletspam.LoadCode(walletspam.CodeHex)
	jwCode := walletspam.LoadJettonWalletCode()
	if pool.ExpectedWallet0 != "" {
		wallet0 := walletspam.New(0, ed25519.PublicKey(pub), nil, minter, jwCode, code).Address().StringRaw()
		expected, err := parseTonAddress(pool.ExpectedWallet0)
		if err != nil || wallet0 != expected.StringRaw() {
			return Fuel{}, fmt.Errorf("public pool configuration does not match expected wallet 0")
		}
	}
	ids := sampleIDs(pub, pool.Count, 20)
	owners := make([]string, len(ids))
	jettons := make([]string, len(ids))
	query := url.Values{"include_boc": {"false"}}
	for i, id := range ids {
		owner := walletspam.New(id, ed25519.PublicKey(pub), nil, minter, jwCode, code).Address()
		owners[i] = owner.StringRaw()
		jettons[i] = jetton.WalletAddress(owner, minter, jwCode).StringRaw()
		query.Add("address", owners[i])
		query.Add("address", jettons[i])
	}
	var data struct {
		Accounts []struct {
			Address string  `json:"address"`
			Balance *string `json:"balance"`
		} `json:"accounts"`
	}
	if err := c.get(ctx, cleanURL(c.cfg.V3URL)+"/accountStates?"+query.Encode(), true, &data); err != nil {
		return Fuel{}, err
	}
	balances := make(map[string]float64, len(data.Accounts))
	for _, item := range data.Accounts {
		if item.Balance == nil {
			continue
		}
		v, err := nanoToGRAM(*item.Balance)
		if err != nil {
			return Fuel{}, err
		}
		addr, err := parseTonAddress(item.Address)
		if err == nil {
			balances[addr.StringRaw()] = v
		}
	}
	fuel := Fuel{Count: pool.Count, Scope: "whole pool", Initial: pool.InitialBalance, Threshold: pool.SendThreshold, Source: "Toncenter v3 /accountStates"}
	if c.cfg.Network == "testnet" {
		fuel.Scope = "whole pool; active subpool unknown"
	}
	values := make([]float64, 0, len(ids))
	for i := range ids {
		owner, ok := balances[owners[i]]
		if !ok {
			fuel.Missing++
			continue
		}
		fuel.Sampled++
		values = append(values, owner)
		fuel.Average += owner
		if owner < pool.SendThreshold {
			fuel.BelowThreshold++
		}
		if jettonBalance, ok := balances[jettons[i]]; ok {
			fuel.JettonAverage += jettonBalance
		} else {
			fuel.Missing++
		}
	}
	if fuel.Sampled == 0 {
		return Fuel{}, fmt.Errorf("none of the sampled pool wallets were found by the indexer")
	}
	fuel.Average /= float64(fuel.Sampled)
	fuel.JettonAverage /= float64(fuel.Sampled)
	sort.Float64s(values)
	fuel.P10 = values[int(math.Floor(float64(len(values)-1)*0.1))]
	fuel.RemainingPct = math.Max(0, math.Min(100, 100*(fuel.Average-pool.SendThreshold)/(pool.InitialBalance-pool.SendThreshold)))
	if fuel.Missing == 0 && c.cfg.Network == "mainnet" {
		now := time.Now()
		if len(c.fuelTrend) > 0 && fuel.Average > c.fuelTrend[len(c.fuelTrend)-1].avg+0.01 {
			c.fuelTrend = nil // A top-up or redeploy starts a new depletion curve.
		}
		c.fuelTrend = append(c.fuelTrend, fuelPoint{at: now, avg: fuel.Average})
		if len(c.fuelTrend) > 15 {
			c.fuelTrend = c.fuelTrend[len(c.fuelTrend)-15:]
		}
		if len(c.fuelTrend) >= 4 {
			oldest := c.fuelTrend[0]
			minutes := now.Sub(oldest.at).Minutes()
			rate := (oldest.avg - fuel.Average) / minutes
			if minutes >= 3 && rate > 0.00001 && fuel.Average > pool.SendThreshold {
				remaining := (fuel.Average - pool.SendThreshold) / rate
				if remaining >= 0 && remaining < 1e5 {
					fuel.MinutesLeft = &remaining
				}
			}
		}
	}
	return fuel, nil
}
