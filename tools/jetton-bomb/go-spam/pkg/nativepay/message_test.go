package nativepay

import (
	"bytes"
	"crypto/ed25519"
	"encoding/binary"
	"math"
	"strconv"
	"testing"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

func fixtureKey() ed25519.PrivateKey { return ed25519.NewKeyFromSeed(bytes.Repeat([]byte{0x42}, 32)) }
func fixtureDomain() []byte          { return bytes.Repeat([]byte{0x51}, 32) }
func destination(value byte) *address.Address {
	return address.NewAddress(0, 0, bytes.Repeat([]byte{value}, 32))
}
func outputs(n int) []Output {
	out := make([]Output, n)
	for i := range out {
		out[i] = Output{destination(byte(i + 1)), uint64(i + 1), uint64(i)}
	}
	return out
}

func TestScalarWireAndDomainSignature(t *testing.T) {
	key := fixtureKey()
	output := Output{destination(0x31), 123, 7}
	root, err := BuildMessage(key, []Output{output}, 19, math.MaxUint32, fixtureDomain(), false, 0)
	if err != nil {
		t.Fatal(err)
	}
	s := root.BeginParse()
	if s.MustLoadUInt(32) != TransferMagic {
		t.Fatal("not a raw NTXF root")
	}
	if !bytes.Equal(s.MustLoadSlice(256), key.Public().(ed25519.PublicKey)) || !bytes.Equal(s.MustLoadSlice(256), output.Destination.Data()) {
		t.Fatal("source/destination mismatch")
	}
	for _, want := range []uint64{123, 7, 19} {
		if s.MustLoadUInt(64) != want {
			t.Fatal("uint64 field mismatch")
		}
	}
	if s.MustLoadUInt(32) != math.MaxUint32 {
		t.Fatal("expiry mismatch")
	}
	signature := s.MustLoadRef().MustLoadSlice(512)
	if s.BitsLeft() != 0 || s.RefsNum() != 0 {
		t.Fatal("unexpected wire suffix")
	}
	// Independently construct byte payload with binary.Write field semantics.
	payload := append([]byte("TON_NATIVE_TRANSFER_V1"), fixtureDomain()...)
	b := new(bytes.Buffer)
	binary.Write(b, binary.BigEndian, uint32(TransferMagic))
	b.Write(key.Public().(ed25519.PublicKey))
	b.Write(output.Destination.Data())
	binary.Write(b, binary.BigEndian, uint64(123))
	binary.Write(b, binary.BigEndian, uint64(7))
	binary.Write(b, binary.BigEndian, uint64(19))
	binary.Write(b, binary.BigEndian, uint32(math.MaxUint32))
	payload = append(payload, b.Bytes()...)
	if !ed25519.Verify(key.Public().(ed25519.PublicKey), payload, signature) {
		t.Fatal("domain signature does not match raw ordered field bytes")
	}
	payload[len("TON_NATIVE_TRANSFER_V1")] ^= 1
	if ed25519.Verify(key.Public().(ed25519.PublicKey), payload, signature) {
		t.Fatal("signature is replayable across chain domains")
	}
}

func decodeOutputs(t *testing.T, root *cell.Cell, expected int) []Output {
	t.Helper()
	s := root.BeginParse()
	tag := s.MustLoadUInt(32)
	count := int(s.MustLoadUInt(5))
	if tag == OutputsLeafMagic {
		if count < 1 || count > 2 || count != expected {
			t.Fatal("invalid leaf shape")
		}
		out := make([]Output, count)
		for i := range out {
			out[i] = Output{address.NewAddress(0, 0, s.MustLoadSlice(256)), s.MustLoadUInt(64), s.MustLoadUInt(64)}
		}
		if s.BitsLeft() != 0 || s.RefsNum() != 0 {
			t.Fatal("leaf trailing data")
		}
		return out
	}
	if tag != OutputsNodeMagic || count < 1 || count >= expected {
		t.Fatal("invalid output-tree node")
	}
	left, right := s.MustLoadRef().MustToCell(), s.MustLoadRef().MustToCell()
	if s.BitsLeft() != 0 || s.RefsNum() != 0 {
		t.Fatal("node trailing data")
	}
	return append(decodeOutputs(t, left, count), decodeOutputs(t, right, expected-count)...)
}

func TestSignedRunCountOrderAndSignatures(t *testing.T) {
	for _, n := range []int{1, 2, 3, 5, 7, 9, 15, 16} {
		t.Run(strconv.Itoa(n)+" outputs", func(t *testing.T) {
			key := fixtureKey()
			out := outputs(n)
			root, err := BuildMessage(key, out, math.MaxUint64-uint64(n), math.MaxUint32, fixtureDomain(), true, 0)
			if err != nil {
				t.Fatal(err)
			}
			s := root.BeginParse()
			if s.MustLoadUInt(32) != RunMagic {
				t.Fatal("not NTRN")
			}
			if !bytes.Equal(s.MustLoadSlice(256), key.Public().(ed25519.PublicKey)) {
				t.Fatal("source differs")
			}
			nonce, until, count := s.MustLoadUInt(64), s.MustLoadUInt(32), int(s.MustLoadUInt(5))
			if nonce != math.MaxUint64-uint64(n) || until != math.MaxUint32 || count != n {
				t.Fatal("run fields differ")
			}
			signature, tree := s.MustLoadRef().MustLoadSlice(512), s.MustLoadRef().MustToCell()
			if s.BitsLeft() != 0 || s.RefsNum() != 0 {
				t.Fatal("root trailing data")
			}
			decoded := decodeOutputs(t, tree, n)
			for i, value := range decoded {
				if !value.Destination.Equals(out[i].Destination) || value.Amount != out[i].Amount || value.Fee != out[i].Fee {
					t.Fatal("outputs changed order")
				}
			}
			payload := signingPayload(AddressFromKey(key), out, nonce, uint32(until), fixtureDomain(), true)
			if !ed25519.Verify(key.Public().(ed25519.PublicKey), payload, signature) {
				t.Fatal("run signature failed")
			}
			if n > 1 {
				out[0], out[1] = out[1], out[0]
				if ed25519.Verify(key.Public().(ed25519.PublicKey), signingPayload(AddressFromKey(key), out, nonce, uint32(until), fixtureDomain(), true), signature) {
					t.Fatal("output reordering did not invalidate signature")
				}
			}
		})
	}
}

func TestConstructionBoundsAndPaymentLanes(t *testing.T) {
	key := fixtureKey()
	self := Output{AddressFromKey(key), 1, 0}
	cases := []struct {
		name   string
		key    ed25519.PrivateKey
		out    []Output
		nonce  uint64
		until  uint32
		domain []byte
		run    bool
		depth  uint32
	}{
		{"empty", key, nil, 0, 1, fixtureDomain(), true, 0},
		{"17 outputs", key, outputs(17), 0, 1, fixtureDomain(), true, 0},
		{"scalar multiple", key, outputs(2), 0, 1, fixtureDomain(), false, 0},
		{"final nonce overflow", key, []Output{self}, math.MaxUint64, 1, fixtureDomain(), false, 0},
		{"run final nonce overflow", key, outputs(16), math.MaxUint64 - 15, 1, fixtureDomain(), true, 0},
		{"zero amount", key, []Output{{self.Destination, 0, 0}}, 0, 1, fixtureDomain(), true, 0},
		{"debit overflow", key, []Output{{self.Destination, math.MaxUint64, 1}}, 0, 1, fixtureDomain(), true, 0},
		{"aggregate overflow", key, []Output{{self.Destination, math.MaxUint64, 0}, self}, 0, 1, fixtureDomain(), true, 0},
		{"zero expiry", key, []Output{self}, 0, 0, fixtureDomain(), true, 0},
		{"wrong domain length", key, []Output{self}, 0, 1, fixtureDomain()[:31], true, 0},
		{"nil destination", key, []Output{{nil, 1, 0}}, 0, 1, fixtureDomain(), true, 0},
		{"wrong workchain", key, []Output{{address.NewAddress(0, 255, self.Destination.Data()), 1, 0}}, 0, 1, fixtureDomain(), true, 0},
		{"lane depth 61", key, []Output{self}, 0, 1, fixtureDomain(), true, 61},
		{"short key", key[:32], []Output{self}, 0, 1, fixtureDomain(), true, 0},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			if _, err := BuildMessage(test.key, test.out, test.nonce, test.until, test.domain, test.run, test.depth); err == nil {
				t.Fatal("invalid construction accepted")
			}
		})
	}
	for _, depth := range []uint32{1, 2, 8, 13, 60} {
		if _, err := BuildMessage(key, []Output{self}, 0, 1, fixtureDomain(), true, depth); err != nil {
			t.Fatal(err)
		}
	}
	other := append([]byte(nil), self.Destination.Data()...)
	other[0] ^= 0x80
	if _, err := BuildMessage(key, []Output{{address.NewAddress(0, 0, other), 1, 0}}, 0, 1, fixtureDomain(), true, 2); err == nil {
		t.Fatal("cross-lane payment accepted")
	}
	if _, err := BuildMessage(key, []Output{{self.Destination, math.MaxUint64, 0}}, math.MaxUint64-1, 1, fixtureDomain(), true, 0); err != nil {
		t.Fatal("valid maximum uint64 payment rejected:", err)
	}
	broken := append(ed25519.PrivateKey(nil), key...)
	broken[63] ^= 1
	if _, err := BuildMessage(broken, []Output{self}, 0, 1, fixtureDomain(), true, 0); err == nil {
		t.Fatal("inconsistent key pair accepted")
	}
}
