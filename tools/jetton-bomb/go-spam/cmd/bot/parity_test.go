package main

import (
	"crypto/ed25519"
	"encoding/hex"
	"testing"

	"github.com/xssnick/tonutils-go/address"

	"github.com/victor/jetton-bomb-go-spam/pkg/jetton"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

// Cross-toolchain parity for the prepaid jetton-wallet address: Go's jetton.WalletAddress must match the
// Tolk calcAddressOfJettonWallet for owner = WalletSpam#0 of the fixed vector (see
// jetton-prepaid/scripts/_parity.tolk).
func TestJettonWalletAddressParity(t *testing.T) {
	pub, err := hex.DecodeString("0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210")
	if err != nil {
		t.Fatal(err)
	}
	minter := address.MustParseRawAddr("0:0000000000000000000000000000000000000000000000000000000000000abc")
	code := walletspam.LoadCode(walletspam.CodeHex)
	jwCode := walletspam.LoadJettonWalletCode()

	owner := walletspam.New(0, ed25519.PublicKey(pub), nil, minter, jwCode, code).Address()
	jw := jetton.WalletAddress(owner, minter, jwCode)

	const wantHash = "7893edce81c05d72054c84b53fe083b37136d5558cf70670d7fda7d73b772b4a"
	gotHash := hex.EncodeToString(jw.Data())
	if gotHash != wantHash || jw.Workchain() != 0 {
		t.Fatalf("jetton-wallet address mismatch (Go vs Tolk):\n got  %d:%s\n want 0:%s",
			jw.Workchain(), gotHash, wantHash)
	}
}
