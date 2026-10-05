package deploy

import (
	"crypto/ed25519"
	"encoding/hex"
	"math/big"
	"testing"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"

	"github.com/victor/jetton-bomb-go-spam/pkg/jetton"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

// dec renders a 32-byte cell/address hash as the decimal uint256 that acton's println prints, so the
// golden values below are copied verbatim from `acton script scripts/_deploy_parity.tolk`.
func dec(b []byte) string { return new(big.Int).SetBytes(b).String() }

// TestKickoffParity asserts the Go native deploy reproduces the acton buildKickoffExternal output
// byte-for-byte for a fixed vector. Golden from jetton-prepaid/scripts/_deploy_parity.tolk — re-run it
// and update these constants if WalletSpam/JettonWallet code, the Reproduce struct, or the kickoff
// layout changes (that shifts every address + the signed external).
func TestKickoffParity(t *testing.T) {
	seed, err := hex.DecodeString("0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210")
	if err != nil {
		t.Fatal(err)
	}
	priv := ed25519.NewKeyFromSeed(seed)
	pub := priv.Public().(ed25519.PublicKey)
	minter := address.MustParseRawAddr("0:0000000000000000000000000000000000000000000000000000000000000abc")
	code := walletspam.LoadCode(walletspam.CodeHex)
	jwCode := walletspam.LoadJettonWalletCode()

	d := New(pub, priv, minter, jwCode, code, Params{
		Count: 5, Chunk: 2, DeployJetton: true,
		WalletValue: tlb.FromNanoTON(big.NewInt(200000000)),
		JettonValue: tlb.FromNanoTON(big.NewInt(10000000)),
		FeeSlack:    tlb.FromNanoTON(big.NewInt(10000000)),
	})

	if got := dec(d.Wallet0().Address().Data()); got != "109969901096953121033576609341691688765629722960139725202150162572554829056165" {
		t.Fatalf("wallet0 hash: %s", got)
	}
	// The external attaches Wallet0().StateInit() to deploy wallet0; tlb.ToCell of it MUST hash to
	// wallet0's address or TON rejects the deploy (init hash must equal the destination). This guards
	// the HTTP backend's SendExternalMessageWithInit serialization path.
	siCell, err := tlb.ToCell(d.Wallet0().StateInit())
	if err != nil {
		t.Fatal(err)
	}
	if dec(siCell.Hash()) != dec(d.Wallet0().Address().Data()) {
		t.Fatalf("wallet0 state_init cell hash %s != address %s", dec(siCell.Hash()), dec(d.Wallet0().Address().Data()))
	}
	w1 := walletspam.New(1, pub, priv, minter, jwCode, code)
	if got := dec(w1.Address().Data()); got != "8707918589102844076231898525437840154019611923868262963837985402429666005309" {
		t.Fatalf("wallet1 hash: %s", got)
	}
	jw := jetton.WalletAddress(d.Wallet0().Address(), minter, jwCode)
	if got := dec(jw.Data()); got != "55893615461856395733373621470177336232194921874146902884110347427553982313909" {
		t.Fatalf("wallet0 jetton hash: %s", got)
	}

	if got := d.Wallet0Funding().Nano().String(); got != "1260000000" {
		t.Fatalf("wallet0 funding: %s", got)
	}
	if got := d.reproFunding().Nano().String(); got != "1050000000" {
		t.Fatalf("repro funding: %s", got)
	}

	body, err := d.KickoffBody()
	if err != nil {
		t.Fatal(err)
	}
	const wantKickoff = "45537796334800481502913967270046471399370761941023003072789990120324157743345"
	if got := dec(body.Hash()); got != wantKickoff {
		t.Fatalf("kickoff external hash drift (Go vs Tolk):\n got  %s\n want %s", got, wantKickoff)
	}
}
