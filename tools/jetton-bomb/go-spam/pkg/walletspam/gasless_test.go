package walletspam

import (
	"crypto/ed25519"
	"encoding/hex"
	"math/big"
	"testing"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

// Ground truth: `acton script scripts/_gasless_parity.tolk`.
func TestInternalSignedRequestParity(t *testing.T) {
	seed, err := hex.DecodeString("0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210")
	if err != nil {
		t.Fatal(err)
	}
	priv := ed25519.NewKeyFromSeed(seed)
	pub := priv.Public().(ed25519.PublicKey)
	minter := address.MustParseRawAddr("0:0000000000000000000000000000000000000000000000000000000000000abc")
	destination := address.MustParseRawAddr("0:0000000000000000000000000000000000000000000000000000000000000def")
	wallet := New(7, pub, priv, minter, LoadJettonWalletCode(), LoadCode(CodeHex))

	outMessage, err := tlb.ToCell(&tlb.InternalMessage{
		IHRDisabled: true,
		Bounce:      false,
		DstAddr:     destination,
		Amount:      tlb.FromNanoTONU(1234567),
		Body:        cell.BeginCell().EndCell(),
	})
	if err != nil {
		t.Fatal(err)
	}
	body, err := wallet.BuildInternalSignedRequest(9, 0xFFFFFFFF, []Action{{
		Mode:    DefaultSendMode,
		Message: outMessage,
	}})
	if err != nil {
		t.Fatal(err)
	}

	const want = "30803001477858239555777370560565445504176582867959629348398957607531907988313"
	if got := new(big.Int).SetBytes(body.Hash()).String(); got != want {
		t.Fatalf("signed-internal body hash mismatch (Go vs Tolk):\n got  %s\n want %s", got, want)
	}

	relayer := New(0, pub, priv, minter, LoadJettonWalletCode(), LoadCode(CodeHex))
	outer, err := BuildRelayedExternal(
		relayer,
		3,
		wallet,
		9,
		0xFFFFFFFF,
		tlb.FromNanoTONU(10_000_000),
		[]Action{{Mode: DefaultSendMode, Message: outMessage}},
	)
	if err != nil {
		t.Fatal(err)
	}
	const wantOuter = "69847034533761213953522373083805071075486712569359632107226579401970348276875"
	if got := new(big.Int).SetBytes(outer.Hash()).String(); got != wantOuter {
		t.Fatalf("relayed external body hash mismatch (Go vs Tolk):\n got  %s\n want %s", got, wantOuter)
	}
}
