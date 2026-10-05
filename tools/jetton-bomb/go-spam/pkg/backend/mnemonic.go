package backend

import (
	"crypto/ed25519"
	"encoding/hex"
	"fmt"
	"os"
	"strings"

	"github.com/xssnick/tonutils-go/ton/wallet"
)

// LoadKeypairFromFile reads a TON wallet mnemonic from path (whitespace-separated words)
// and derives the ed25519 keypair via tonutils-go.
func LoadKeypairFromFile(path string) (ed25519.PublicKey, ed25519.PrivateKey, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, fmt.Errorf("read mnemonic file %s: %w", path, err)
	}
	return LoadKeypairFromMnemonic(string(raw))
}

// DefaultPoolKeyFile picks the first existing jetton-prepaid/pool.key candidate relative to the CWD —
// so the bot finds it whether it runs from the repo root (WorkingDirectory=/…/jetton-bomb) or from
// go-spam/. If none exist yet it returns the repo-root-relative path. Override with POOL_KEY_FILE.
func DefaultPoolKeyFile() string {
	for _, c := range []string{"./jetton-prepaid/pool.key", "../jetton-prepaid/pool.key", "./pool.key"} {
		if _, err := os.Stat(c); err == nil {
			return c
		}
	}
	return "./jetton-prepaid/pool.key"
}

// LoadKeypairFromSeedFile reads a raw ed25519 secret from a file — the 32-byte seed the acton deploy's
// loadPoolKeyPair writes to jetton-prepaid/pool.key — and derives the keypair. acton's
// crypto.seedToKeyPair and ed25519.NewKeyFromSeed agree on the same 32 bytes, so the derived pubkey (and
// every pool address) matches across toolchains. No mnemonic involved.
func LoadKeypairFromSeedFile(path string) (ed25519.PublicKey, ed25519.PrivateKey, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, fmt.Errorf("read pool key file %s: %w", path, err)
	}
	return LoadKeypairFromSecret(raw)
}

// LoadKeypairFromSecretHex accepts a hex-encoded ed25519 secret: either a 32-byte seed or a full
// 64-byte private key (seed||pub). Convenient for pasting a key derived from a mnemonic (see cmd/derivekey).
func LoadKeypairFromSecretHex(s string) (ed25519.PublicKey, ed25519.PrivateKey, error) {
	raw, err := hex.DecodeString(strings.TrimSpace(s))
	if err != nil {
		return nil, nil, fmt.Errorf("decode secret hex: %w", err)
	}
	return LoadKeypairFromSecret(raw)
}

// LoadKeypairFromSecret derives the keypair from a raw ed25519 secret: a 32-byte seed
// (ed25519.NewKeyFromSeed) or a 64-byte private key (seed||pub, used directly).
func LoadKeypairFromSecret(raw []byte) (ed25519.PublicKey, ed25519.PrivateKey, error) {
	switch len(raw) {
	case ed25519.SeedSize: // 32
		priv := ed25519.NewKeyFromSeed(raw)
		return priv.Public().(ed25519.PublicKey), priv, nil
	case ed25519.PrivateKeySize: // 64
		priv := ed25519.PrivateKey(append([]byte(nil), raw...))
		return priv.Public().(ed25519.PublicKey), priv, nil
	default:
		return nil, nil, fmt.Errorf("ed25519 secret must be 32 (seed) or 64 (private key) bytes, got %d", len(raw))
	}
}

// LoadKeypairFromMnemonic accepts a whitespace-separated mnemonic string.
func LoadKeypairFromMnemonic(mnemonic string) (ed25519.PublicKey, ed25519.PrivateKey, error) {
	words := strings.Fields(strings.TrimSpace(mnemonic))
	if len(words) == 0 {
		return nil, nil, fmt.Errorf("empty mnemonic")
	}
	priv, err := wallet.SeedToPrivateKey(words, "", false)
	if err != nil {
		return nil, nil, fmt.Errorf("derive key: %w", err)
	}
	pub := priv.Public().(ed25519.PublicKey)
	return pub, priv, nil
}
