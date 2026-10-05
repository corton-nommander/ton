package nativepay

import (
	"bytes"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"math"
	"os"
	"strconv"
	"strings"
	"testing"

	"github.com/xssnick/tonutils-go/tvm/cell"
)

// The oracle uses the independent C++ tonlib::native_client::create_message
// implementation, with fields/signatures parsed back by block::NativeTransfer
// and NativeTransferRun. These fixtures are public deterministic test keys.
func TestCppNativeClientParity(t *testing.T) {
	data, err := os.ReadFile("testdata/cpp-native.json")
	if err != nil {
		t.Fatal(err)
	}
	var fixtures []struct {
		Kind      string `json:"kind"`
		Count     int    `json:"count"`
		Source    string `json:"source"`
		Payload   string `json:"payload"`
		Signature string `json:"signature"`
		RootHash  string `json:"root_hash"`
		BOC       string `json:"boc"`
	}
	if err := json.Unmarshal(data, &fixtures); err != nil {
		t.Fatal(err)
	}
	for _, fixture := range fixtures {
		t.Run(fixture.Kind+"/"+strconv.Itoa(fixture.Count), func(t *testing.T) {
			key := fixtureKey()
			out := outputs(fixture.Count)
			run := fixture.Kind == "run"
			if !run {
				out = []Output{{destination(0x31), 123, 7}}
			}
			root, err := BuildMessage(key, out, 19, math.MaxUint32, fixtureDomain(), run, 0)
			if err != nil {
				t.Fatal(err)
			}
			if !strings.EqualFold(hex.EncodeToString(AddressFromKey(key).Data()), fixture.Source) {
				t.Fatal("source account differs from C++")
			}
			payload := signingPayload(AddressFromKey(key), out, 19, math.MaxUint32, fixtureDomain(), run)
			if hex.EncodeToString(payload) != fixture.Payload {
				t.Fatal("signed raw field payload differs from C++")
			}
			if hex.EncodeToString(ed25519.Sign(key, payload)) != fixture.Signature {
				t.Fatal("Ed25519 signature differs from C++")
			}
			if !strings.EqualFold(hex.EncodeToString(root.Hash()), fixture.RootHash) {
				t.Fatalf("canonical root differs from C++: %x != %s", root.Hash(), fixture.RootHash)
			}
			boc, err := hex.DecodeString(fixture.BOC)
			if err != nil {
				t.Fatal(err)
			}
			cppRoot, err := cell.FromBOC(boc)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(root.Hash(), cppRoot.Hash()) {
				t.Fatal("retained C++ BOC root does not match Go")
			}
		})
	}
}
