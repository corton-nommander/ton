package fats

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"

	"github.com/xssnick/tonutils-go/address"
)

// Manifest is the subset of the state generator's manifest.json (ton/benchmark/common.cpp Manifest)
// that go-spam needs to reconstruct the generated wallets: the shared derivation seed, the wallet count
// and wallet_id, and the minter address that fixes every jetton-wallet address.
type Manifest struct {
	SeedHex       string `json:"seed_hex"`
	NumV5         uint64 `json:"num_v5"`
	NumFats       uint64 `json:"num_fats"`
	FatsSize      int    `json:"fats_size"`
	WalletID      int32  `json:"wallet_id"`
	MinterAddrHex string `json:"minter_addr_hex"`
}

// LoadManifest reads and parses a state-gen manifest.json.
func LoadManifest(path string) (*Manifest, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read manifest %s: %w", path, err)
	}
	var m Manifest
	if err := json.Unmarshal(data, &m); err != nil {
		return nil, fmt.Errorf("parse manifest %s: %w", path, err)
	}
	return &m, nil
}

// Seed decodes the 32-byte derivation seed.
func (m *Manifest) Seed() ([32]byte, error) {
	var s [32]byte
	b, err := hex.DecodeString(m.SeedHex)
	if err != nil {
		return s, fmt.Errorf("bad seed_hex %q: %w", m.SeedHex, err)
	}
	if len(b) != 32 {
		return s, fmt.Errorf("seed_hex must be 32 bytes, got %d", len(b))
	}
	copy(s[:], b)
	return s, nil
}

// Minter decodes the minter address (basechain, workchain 0).
func (m *Manifest) Minter() (*address.Address, error) {
	b, err := hex.DecodeString(m.MinterAddrHex)
	if err != nil {
		return nil, fmt.Errorf("bad minter_addr_hex %q: %w", m.MinterAddrHex, err)
	}
	if len(b) != 32 {
		return nil, fmt.Errorf("minter_addr_hex must be 32 bytes, got %d", len(b))
	}
	return address.NewAddress(0, 0, b), nil
}
