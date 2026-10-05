package walletspam

import (
	"crypto/ed25519"
	"encoding/hex"
	"testing"

	"github.com/xssnick/tonutils-go/address"
)

// Cross-toolchain address parity: Go must compute the SAME WalletSpam#0 address as the Tolk
// contract/deploy script for a fixed (pubkey, minter, jwCode) vector. Ground truth was produced by
// jetton-prepaid/scripts/_parity.tolk (walletSpamAddress(0, ...) with build("JettonWallet")).
//
//	pubkey = 0x0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210
//	minter = 0:00...0abc
func TestWalletSpamAddressParity(t *testing.T) {
	pub, err := hex.DecodeString("0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210")
	if err != nil {
		t.Fatal(err)
	}
	minter := address.MustParseRawAddr("0:0000000000000000000000000000000000000000000000000000000000000abc")
	code := LoadCode(CodeHex)
	jwCode := LoadJettonWalletCode()

	w := New(0, ed25519.PublicKey(pub), nil, minter, jwCode, code)

	const wantHash = "0817a71b21bc26b0b935965f190b524568536d84f23bae4178b748b075db235c"
	gotHash := hex.EncodeToString(w.Address().Data())
	if gotHash != wantHash || w.Address().Workchain() != 0 {
		t.Fatalf("WalletSpam#0 address mismatch (Go vs Tolk):\n got  %d:%s\n want 0:%s",
			w.Address().Workchain(), gotHash, wantHash)
	}
}
