package walletv5

import (
	"encoding/hex"
	"testing"
)

// Cross-toolchain parity: the deterministically-derived bench wallet-v5 address (and the fat load-target
// address) must match ton/benchmark/state-gen.cpp byte-for-byte, or transfers target uninitialized
// accounts. Golden vectors captured from:
//
//	bench-state-gen derive --seed-hex 0101..01 --index {0,7}
//
// (wallet_id 0). Regenerate these constants if the W5 code, data layout, or fat derivation changes.
func TestBenchW5Parity(t *testing.T) {
	seed := hexTo32("0101010101010101010101010101010101010101010101010101010101010101")
	cases := []struct {
		idx          uint64
		pub, w5, fat string
	}{
		{
			idx: 0,
			pub: "fa7a9e0f6e93688a91a87a6d09549ca8c28ba1390c342c5855771ea5fac61300",
			w5:  "05e53d249e436e91eff21172593390966b0da963430f947e8aeb692487d8a93f",
			fat: "01c8618d91fdee38f56597e5b8349d4e32d88a8b53fcc4032771c5bfc4c09200",
		},
		{
			idx: 7,
			pub: "0c1b202805394a06b4acbf68e4af95bf7e43d38780b594d70129d822640455fe",
			w5:  "bfe1ed2592f00f399045e6c6d6e55495226cf1f6f6ae912e462e1ab38f55db8d",
			fat: "f7c626b20b4569a04f4d596dd83ead3a319cd55673ca802347c72c68dd2bfadf",
		},
	}
	for _, c := range cases {
		w := DeriveW5(seed, c.idx, 0)
		if got := hex.EncodeToString(w.PublicKey()); got != c.pub {
			t.Errorf("idx %d pubkey: got %s want %s", c.idx, got, c.pub)
		}
		if wc := w.Address().Workchain(); wc != 0 {
			t.Errorf("idx %d workchain: got %d want 0", c.idx, wc)
		}
		if got := hex.EncodeToString(w.Address().Data()); got != c.w5 {
			t.Errorf("idx %d w5_addr: got %s want %s", c.idx, got, c.w5)
		}
		if got := hex.EncodeToString(FatAddress(seed, c.idx).Data()); got != c.fat {
			t.Errorf("idx %d fat_addr: got %s want %s", c.idx, got, c.fat)
		}
	}
}

func hexTo32(s string) [32]byte {
	var a [32]byte
	b, err := hex.DecodeString(s)
	if err != nil {
		panic(err)
	}
	copy(a[:], b)
	return a
}
