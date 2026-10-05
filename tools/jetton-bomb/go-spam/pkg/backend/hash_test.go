package backend

import (
	"encoding/base64"
	"testing"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

func TestExternalHashNormMatchesSentHTTPMessage(t *testing.T) {
	bits := make([]byte, 32)
	bits[0], bits[31] = 0xa5, 0x5a
	dst := address.NewAddress(0, 0, bits)
	body := cell.BeginCell().MustStoreUInt(0x73696e74, 32).MustStoreRef(
		cell.BeginCell().MustStoreUInt(42, 64).EndCell()).EndCell()
	wire, err := cell.FromBOC(buildExtMsgBoc(dst, body, nil))
	if err != nil {
		t.Fatal(err)
	}
	want := base64.StdEncoding.EncodeToString(wire.Hash())
	if got := ExternalHashNorm(dst, body); got != want {
		t.Fatalf("normalized hash %q differs from the message posted over HTTP %q", got, want)
	}
}
