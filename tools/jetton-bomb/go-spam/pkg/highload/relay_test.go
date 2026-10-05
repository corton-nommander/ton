package highload

import (
	"crypto/ed25519"
	"testing"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

func testKey(t *testing.T, b byte) (ed25519.PublicKey, ed25519.PrivateKey) {
	t.Helper()
	seed := make([]byte, 32)
	for i := range seed {
		seed[i] = b ^ byte(i)
	}
	priv := ed25519.NewKeyFromSeed(seed)
	return priv.Public().(ed25519.PublicKey), priv
}

// A target must be relayed by the relayer sharing its top two bits, so the hop stays intra-shard.
// A routing bug here would still produce valid externals, just ones that cost an extra cross-shard hop —
// invisible except as degraded throughput, hence the explicit test.
func TestBuildRelayForTargetRoutesIntraShard(t *testing.T) {
	pub, priv := testKey(t, 0x11)
	set := NewShardSet(pub, priv, DefaultTimeout)

	minter := address.MustParseAddr("kQCy3cWuUXredDmXUTipDo31Juda2Y10jfK9DYS0gBPAjD-X")
	jwCode := cell.BeginCell().MustStoreUInt(0xabcd, 16).EndCell()
	spamCode := cell.BeginCell().MustStoreUInt(0x1234, 16).EndCell()
	action := cell.BeginCell().MustStoreUInt(0xdeadbeef, 32).EndCell()

	// Walk enough subwallets to land in every shard bucket at least once.
	covered := map[int]bool{}
	for sub := uint32(0); sub < 400; sub++ {
		target := walletspam.New(sub, pub, priv, minter, jwCode, spamCode)
		want := Shard(target.Address())

		relayerAddr, ext, err := BuildRelayForTarget(
			set, target, 7, 1786800120, tlb.MustFromTON("0.012"), action, 1786800000,
		)
		if err != nil {
			t.Fatalf("subwallet %d: %v", sub, err)
		}
		if got := Shard(relayerAddr); got != want {
			t.Fatalf("subwallet %d: target in shard %x relayed by shard %x (%s)", sub, want, got, relayerAddr)
		}
		if relayerAddr.String() != set[want].Address().String() {
			t.Fatalf("subwallet %d: relayer %s is not set[%x] %s", sub, relayerAddr, want, set[want].Address())
		}
		if ext == nil || len(ext.Hash()) == 0 {
			t.Fatalf("subwallet %d: empty external", sub)
		}
		covered[want] = true
	}
	if len(covered) != NumShards {
		t.Fatalf("only covered %d/%d shard buckets", len(covered), NumShards)
	}
}

// TestBuildRelayForTargetIsDeterministic pins that everything except the relayer's query_id is a pure
// function of the inputs. Both cmd/spam and the bot call this builder, so a hidden dependency on wall
// clock or map order would silently desync the two senders.
func TestBuildRelayForTargetIsDeterministic(t *testing.T) {
	pub, priv := testKey(t, 0x5a)
	minter := address.MustParseAddr("kQCy3cWuUXredDmXUTipDo31Juda2Y10jfK9DYS0gBPAjD-X")
	jwCode := cell.BeginCell().MustStoreUInt(0xabcd, 16).EndCell()
	spamCode := cell.BeginCell().MustStoreUInt(0x1234, 16).EndCell()
	action := cell.BeginCell().MustStoreUInt(0xfeed, 32).EndCell()

	build := func() (*address.Address, *cell.Cell) {
		// Fresh shard set per call with the query-id allocators pinned, so the only source of variation
		// left is the builder itself (production seeds query_id randomly — see randomQueryIDStart).
		set := NewShardSet(pub, priv, DefaultTimeout)
		for _, hl := range set {
			hl.SetQueryIDStart(0)
		}
		target := walletspam.New(42, pub, priv, minter, jwCode, spamCode)
		addr, ext, err := BuildRelayForTarget(
			set, target, 3, 1786800120, tlb.MustFromTON("0.012"), action, 1786800000,
		)
		if err != nil {
			t.Fatalf("build: %v", err)
		}
		return addr, ext
	}

	addr1, ext1 := build()
	addr2, ext2 := build()
	if addr1.String() != addr2.String() {
		t.Fatalf("relayer differs: %s vs %s", addr1, addr2)
	}
	if string(ext1.Hash()) != string(ext2.Hash()) {
		t.Fatalf("external differs: %x vs %x", ext1.Hash(), ext2.Hash())
	}
}

// TestCreatedAtNowIsBackdated pins the fix for the exit-code-35 storm: the contract checks
// `created_at <= now()` against the BLOCK's gen_utime, which trails wall clock by up to a block interval,
// so a wall-clock stamp is rejected most of the time (~22-30% accepted, measured on testnet 17.08).
// created_at must be strictly behind wall clock, and by far less than the timeout (the other bound).
func TestCreatedAtNowIsBackdated(t *testing.T) {
	wall := uint32(time.Now().Unix())
	got := CreatedAtNow()
	if got >= wall {
		t.Fatalf("created_at %d is not behind wall clock %d — node will reject with exit 35", got, wall)
	}
	if lag := wall - got; lag < 5 || lag > 120 {
		t.Fatalf("back-date %ds is outside the sane 5..120s band", lag)
	}
	if CreatedAtBackdateSec >= DefaultTimeout {
		t.Fatalf("back-date %ds exceeds the timeout %ds — every relay would be 'too old'",
			CreatedAtBackdateSec, DefaultTimeout)
	}
}

// TestQueryIDStartIsRandomized pins the fix for the exit-code-36 (already_executed) storm: the on-chain
// replay dict remembers ids for up to 2*timeout, so a counter restarting at 0 replays whatever the
// previous process spent. Two freshly built wallets for the SAME key must not hand out the same first id,
// and the start must stay inside the valid index space.
func TestQueryIDStartIsRandomized(t *testing.T) {
	pub, priv := testKey(t, 0x3c)

	seen := map[uint32]int{}
	const n = 200
	for i := 0; i < n; i++ {
		w := New(1, pub, priv, DefaultTimeout)
		qid := w.nextQueryID()
		if qid >= 1<<23 {
			t.Fatalf("query_id %d exceeds 2^23", qid)
		}
		if qid&1023 == 1023 {
			t.Fatalf("query_id %d has reserved bit_number 1023", qid)
		}
		seen[qid]++
	}
	// A fixed start would collapse all n to one value; random starts over 8.38M ids collide ~never.
	if len(seen) < n-1 {
		t.Fatalf("only %d distinct first query_ids out of %d — start is not randomized", len(seen), n)
	}
}

// TestBuildRelayQueryIDAdvances guards the opposite property: consecutive relays from the SAME relayer
// must differ, or the second one is a replay the contract rejects.
func TestBuildRelayQueryIDAdvances(t *testing.T) {
	pub, priv := testKey(t, 0x77)
	set := NewShardSet(pub, priv, DefaultTimeout)
	minter := address.MustParseAddr("kQCy3cWuUXredDmXUTipDo31Juda2Y10jfK9DYS0gBPAjD-X")
	jwCode := cell.BeginCell().MustStoreUInt(0xabcd, 16).EndCell()
	spamCode := cell.BeginCell().MustStoreUInt(0x1234, 16).EndCell()
	action := cell.BeginCell().MustStoreUInt(0x11, 8).EndCell()
	target := walletspam.New(9, pub, priv, minter, jwCode, spamCode)

	_, ext1, err := BuildRelayForTarget(set, target, 1, 1786800120, tlb.MustFromTON("0.012"), action, 1786800000)
	if err != nil {
		t.Fatalf("first: %v", err)
	}
	_, ext2, err := BuildRelayForTarget(set, target, 2, 1786800120, tlb.MustFromTON("0.012"), action, 1786800000)
	if err != nil {
		t.Fatalf("second: %v", err)
	}
	if string(ext1.Hash()) == string(ext2.Hash()) {
		t.Fatal("two relays from the same relayer produced an identical external (query_id did not advance)")
	}
}
