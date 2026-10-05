package fats

import (
	"encoding/hex"
	"testing"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletv5"
)

// Cross-toolchain parity for the prepaid jetton-wallet address of a generated wallet-v5 wallet. Go's
// WalletAddress(owner, minter, code) must match ton/benchmark/state-gen.cpp's jw_addr, or transfers
// route to uninitialized accounts. Golden vectors from:
//
//	bench-state-gen derive --seed-hex 0101..01 --index {0,7}
//
// The minter is taken as an input (in production go-spam reads it from manifest.json); this test only
// checks Go's JW-address FORMULA against the generator's for that same minter.
func TestBenchJWParity(t *testing.T) {
	seed := hexTo32("0101010101010101010101010101010101010101010101010101010101010101")
	minter := address.NewAddress(0, 0, hexToBytes("8bb821a8fb86d08ea95870104809625b266e43b1e5bab10764e8852d20bc6681"))
	jwCode, err := cell.FromBOC(hexToBytes(walletspam.JettonWalletCodeHex))
	if err != nil {
		t.Fatalf("parse jw code: %v", err)
	}
	cases := []struct {
		idx uint64
		jw  string
	}{
		{0, "80c160660098216a24dcbb2714234f7f408ae8f8ea9fd5379a190198b1008f2e"},
		{7, "82f653ff755839b0608bd18a63126360a9bf476659f9e2598960ded572ff5cc5"},
	}
	for _, c := range cases {
		w5 := walletv5.DeriveW5(seed, c.idx, 0)
		jw := WalletJettonAddress(w5.Address(), minter, jwCode)
		if got := hex.EncodeToString(jw.Data()); got != c.jw {
			t.Errorf("idx %d jw_addr: got %s want %s", c.idx, got, c.jw)
		}
	}
}

func hexTo32(s string) [32]byte {
	var a [32]byte
	copy(a[:], hexToBytes(s))
	return a
}

func hexToBytes(s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		panic(err)
	}
	return b
}
