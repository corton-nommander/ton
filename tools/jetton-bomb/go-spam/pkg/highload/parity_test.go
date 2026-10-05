package highload

import (
	"context"
	"crypto/ed25519"
	"testing"

	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/ton/wallet"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

// TestParityWithTonutilsHighloadV3 locks our wrapper to tonutils' network-proven highload-v3 builder:
// same key/subwallet/ttl must yield the same ADDRESS, and the same (message, mode, query_id, created_at)
// must yield the same EXTERNAL body. tonutils is the oracle because its code constant hashes identical to
// the on-chain Fragment code (see TestCodeHashVsCanonical).
func TestParityWithTonutilsHighloadV3(t *testing.T) {
	seed := make([]byte, 32)
	for i := range seed {
		seed[i] = byte(i * 7)
	}
	priv := ed25519.NewKeyFromSeed(seed)
	pub := priv.Public().(ed25519.PublicKey)

	const ttl = uint32(3600)
	const sub = uint32(777)
	const createdAt = int64(1786800000)
	const fixedQID = uint32(0) // pinned below via SetQueryIDStart; production starts at a random index

	ref, err := wallet.FromPrivateKey(nil, priv, wallet.ConfigHighloadV3{
		MessageTTL: ttl,
		MessageBuilder: func(_ context.Context, _ uint32) (uint32, int64, error) {
			return fixedQID, createdAt, nil
		},
	})
	if err != nil {
		t.Fatalf("ref wallet: %v", err)
	}
	ref, err = ref.GetSubwallet(sub)
	if err != nil {
		t.Fatalf("ref subwallet: %v", err)
	}

	mine := New(sub, pub, priv, ttl)
	mine.SetQueryIDStart(uint64(fixedQID)) // match the oracle's fixed query_id

	// --- address parity ---
	if mine.Address().String() != ref.Address().String() {
		t.Fatalf("address mismatch:\n mine=%s\n ref =%s", mine.Address(), ref.Address())
	}
	t.Logf("address parity OK: %s", mine.Address())

	// --- external parity ---
	internal := &tlb.InternalMessage{
		IHRDisabled: true,
		Bounce:      true,
		DstAddr:     ref.Address(),
		Amount:      tlb.MustFromTON("0.05"),
		Body:        cell.BeginCell().MustStoreUInt(0xdeadbeef, 32).EndCell(),
	}
	msgCell, err := tlb.ToCell(internal)
	if err != nil {
		t.Fatalf("serialize internal: %v", err)
	}

	refExt, err := ref.PrepareExternalMessageForMany(context.Background(), false, []*wallet.Message{{
		Mode:            SendModeRelay,
		InternalMessage: internal,
	}})
	if err != nil {
		t.Fatalf("ref external: %v", err)
	}

	myExt, err := mine.BuildRelayExternal(msgCell, SendModeRelay, uint32(createdAt))
	if err != nil {
		t.Fatalf("my external: %v", err)
	}

	if refExt.Body.Hash() == nil || myExt.Hash() == nil {
		t.Fatal("nil hash")
	}
	if string(myExt.Hash()) != string(refExt.Body.Hash()) {
		t.Fatalf("external body mismatch:\n mine=%x\n ref =%x", myExt.Hash(), refExt.Body.Hash())
	}
	t.Logf("external parity OK: %x", myExt.Hash())
}

// TestGrindShards verifies the four grinded subwallets land in distinct top-two-bit buckets.
func TestGrindShards(t *testing.T) {
	seed := make([]byte, 32)
	for i := range seed {
		seed[i] = byte(255 - i)
	}
	priv := ed25519.NewKeyFromSeed(seed)
	pub := priv.Public().(ed25519.PublicKey)

	subs := GrindSubwallets(pub, DefaultTimeout)
	seen := map[uint32]bool{}
	for bucket := 0; bucket < NumShards; bucket++ {
		if seen[subs[bucket]] {
			t.Fatalf("duplicate subwallet %d", subs[bucket])
		}
		seen[subs[bucket]] = true
		w := New(subs[bucket], pub, priv, DefaultTimeout)
		if got := Shard(w.Address()); got != bucket {
			t.Fatalf("bucket %d: subwallet %d lands in shard %d", bucket, subs[bucket], got)
		}
	}
	t.Logf("grinded four shard subwallets: %v", subs)
}

// TestQueryIDRange checks the allocator stays within valid highload-v3 bounds.
func TestQueryIDRange(t *testing.T) {
	w := New(1, make([]byte, 32), nil, DefaultTimeout)
	for i := 0; i < 5000; i++ {
		qid := w.nextQueryID()
		if qid >= 1<<23 {
			t.Fatalf("query_id %d exceeds 2^23", qid)
		}
		if qid&1023 == 1023 {
			t.Fatalf("query_id %d has reserved bit_number 1023", qid)
		}
	}
}
