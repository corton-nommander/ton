package jetton

import (
	"crypto/ed25519"
	"encoding/hex"
	"testing"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"

	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

// TestBuildTransferExternalStable locks the byte layout of the signed jetton-transfer external for a
// fixed vector. ed25519 signing and cell hashing are both deterministic, so the body hash is stable.
// This guards the shared builder (used by cmd/bot /jetton AND cmd/spam --mode jetton) against silent
// drift — a change here shifts on-chain behavior and must be intentional (update the golden knowingly).
func TestBuildTransferExternalStable(t *testing.T) {
	seed, err := hex.DecodeString("0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210")
	if err != nil {
		t.Fatal(err)
	}
	priv := ed25519.NewKeyFromSeed(seed)
	pub := priv.Public().(ed25519.PublicKey)

	minter := address.MustParseRawAddr("0:0000000000000000000000000000000000000000000000000000000000000abc")
	code := walletspam.LoadCode(walletspam.CodeHex)
	jwCode := walletspam.LoadJettonWalletCode()

	owner := walletspam.New(0, pub, priv, minter, jwCode, code)
	recipient := walletspam.New(1, pub, priv, minter, jwCode, code).Address()

	value := tlb.MustFromTON("0.05")
	body, err := BuildTransferExternal(owner, 0, jwCode, minter, value, 0, recipient, 42, 0xFFFFFFFF)
	if err != nil {
		t.Fatal(err)
	}

	got := hex.EncodeToString(body.Hash())
	const want = "f981d8774e488e0f4bf08e7b2fd6260731809a40d8c8276f2d28e3939a670402"
	if got != want {
		t.Fatalf("jetton transfer external body hash drift:\n got  %s\n want %s", got, want)
	}
}
