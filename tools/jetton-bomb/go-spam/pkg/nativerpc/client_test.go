package nativerpc

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"math"
	"reflect"
	"testing"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tl"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/ton"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

func nativeAccountCell(balance, nonce uint64, flags uint8) *cell.Cell {
	return cell.BeginCell().MustStoreUInt(1, 2).MustStoreUInt(balance, 64).
		MustStoreUInt(nonce, 64).MustStoreUInt(uint64(flags), 8).EndCell()
}

func TestDecodeAccountPreservesNativeUint64Fields(t *testing.T) {
	for _, value := range []uint64{0, 1, 1 << 63, math.MaxUint64} {
		balance, nonce, flags, err := DecodeAccount(nativeAccountCell(value, value, 255))
		if err != nil || balance != value || nonce != value || flags != 255 {
			t.Fatalf("value %d: balance=%d nonce=%d flags=%d err=%v", value, balance, nonce, flags, err)
		}
	}
}

func TestDecodeAccountRejectsAbsentOrdinaryAndMalformedCells(t *testing.T) {
	good := nativeAccountCell(7, 19, 3)
	cases := map[string]*cell.Cell{
		"nil":          nil,
		"account_none": cell.BeginCell().MustStoreUInt(0, 2).EndCell(),
		"truncated":    cell.BeginCell().MustStoreUInt(1, 2).MustStoreUInt(0, 64).MustStoreUInt(0, 64).MustStoreUInt(0, 7).EndCell(),
		"trailing_bit": cell.BeginCell().MustStoreBuilder(good.ToBuilder()).MustStoreUInt(0, 1).EndCell(),
		"trailing_ref": cell.BeginCell().MustStoreBuilder(good.ToBuilder()).MustStoreRef(cell.BeginCell().EndCell()).EndCell(),
		"wrong_tag_00": cell.BeginCell().MustStoreUInt(0, 2).MustStoreUInt(0, 64).MustStoreUInt(0, 64).MustStoreUInt(0, 8).EndCell(),
		"ordinary_10":  cell.BeginCell().MustStoreUInt(2, 2).MustStoreUInt(0, 64).MustStoreUInt(0, 64).MustStoreUInt(0, 8).EndCell(),
		"ordinary_11":  cell.BeginCell().MustStoreUInt(3, 2).MustStoreUInt(0, 64).MustStoreUInt(0, 64).MustStoreUInt(0, 8).EndCell(),
	}
	for name, root := range cases {
		t.Run(name, func(t *testing.T) {
			if _, _, _, err := DecodeAccount(root); err == nil {
				t.Fatal("malformed or nonnative account was accepted")
			}
		})
	}
}

func TestShardContainsAtPrefixBoundaries(t *testing.T) {
	addr := func(prefix uint64) *address.Address {
		data := make([]byte, 32)
		binary.BigEndian.PutUint64(data, prefix)
		return address.NewAddress(0, 0, data)
	}
	rootShard := int64(math.MinInt64)
	for _, prefix := range []uint64{0, 1, math.MaxUint64} {
		if !ShardContains(rootShard, addr(prefix)) {
			t.Fatalf("root shard omitted prefix %016x", prefix)
		}
		if ShardContains(0, addr(prefix)) {
			t.Fatal("zero shard identifier accepted")
		}
	}
	// Four depth-two siblings partition the entire address prefix space.
	shards := []uint64{0x2000000000000000, 0x6000000000000000, 0xa000000000000000, 0xe000000000000000}
	for lane, shard := range shards {
		start := uint64(lane) << 62
		end := start | 0x3fffffffffffffff
		for _, prefix := range []uint64{start, start + 1, end} {
			matches := 0
			for _, other := range shards {
				if ShardContains(int64(other), addr(prefix)) {
					matches++
				}
			}
			if !ShardContains(int64(shard), addr(prefix)) || matches != 1 {
				t.Fatalf("lane %d prefix %016x matched %d leaves", lane, prefix, matches)
			}
		}
	}
	// TON's deepest supported shard has a delimiter at bit three.
	for _, prefix := range []uint64{0x123456789abcdef0, 0x123456789abcdeff} {
		if !ShardContains(0x123456789abcdef8, addr(prefix)) {
			t.Fatalf("depth60 omitted prefix %016x", prefix)
		}
	}
	if ShardContains(0x123456789abcdef8, addr(0x123456789abcdeeF)) {
		t.Fatal("depth60 accepted adjacent address range")
	}
}

func TestSendBatchMatchesCppTLWire(t *testing.T) {
	// C++ auto/tl/lite_api.h assigns sendMessageBatch ID 0xbfa01d43.
	// auto/tl/lite_api.cpp uses a bare vector (length, bytes), without
	// the boxed vector constructor. Strings have four-byte padding.
	want, err := hex.DecodeString("431da0bf020000000301020300000000")
	if err != nil {
		t.Fatal(err)
	}
	got, err := tl.Serialize(SendBatch{Bodies: [][]byte{{1, 2, 3}, {}}}, true)
	if err != nil || !bytes.Equal(got, want) {
		t.Fatalf("C++ batch request mismatch: got %x want %x err=%v", got, want, err)
	}
	var decoded SendBatch
	rest, err := tl.Parse(&decoded, want, true)
	if err != nil || len(rest) != 0 || !reflect.DeepEqual(decoded.Bodies, [][]byte{{1, 2, 3}, {}}) {
		t.Fatalf("batch request decoding mismatch: %v rest=%x err=%v", decoded.Bodies, rest, err)
	}
}

func TestBatchResultMatchesCppBareElementWire(t *testing.T) {
	// sendMsgStatusBatch ID is 0x5bfa767c. C++ TlStoreVector<TlStoreObject>
	// writes concrete SendMsgResult members bare, without their constructor ID.
	want, err := hex.DecodeString("7c76fa5b020000000100000000000000000000000000000007000000026e6f00")
	if err != nil {
		t.Fatal(err)
	}
	value := BatchResult{Results: []SendResult{{Status: 1}, {Code: 7, Message: "no"}}}
	got, err := tl.Serialize(value, true)
	if err != nil || !bytes.Equal(got, want) {
		t.Fatalf("C++ batch response mismatch: got %x want %x err=%v", got, want, err)
	}
	var decoded BatchResult
	rest, err := tl.Parse(&decoded, want, true)
	if err != nil || len(rest) != 0 || !reflect.DeepEqual(decoded, value) {
		t.Fatalf("batch response decoding mismatch: got %#v rest=%x err=%v", decoded, rest, err)
	}
}

func TestProofShardIdentityRequiresWorkchainPrefixDepthAndSeqno(t *testing.T) {
	expected := &ton.BlockIDExt{Workchain: 0, Shard: 0x6000000000000000, SeqNo: 123}
	valid := tlb.ShardIdent{PrefixBits: 2, WorkchainID: 0, ShardPrefix: 0x4000000000000000}
	if !matchesShard(valid, 123, expected) {
		t.Fatal("canonical shard identity rejected")
	}
	wrongWorkchain := valid
	wrongWorkchain.WorkchainID = -1
	wrongPrefix := valid
	wrongPrefix.ShardPrefix = 0
	negativeDepth := valid
	negativeDepth.PrefixBits = -1
	tooDeep := valid
	tooDeep.PrefixBits = 61
	for name, identity := range map[string]tlb.ShardIdent{
		"workchain": wrongWorkchain, "prefix": wrongPrefix,
		"negative_depth": negativeDepth, "depth_over_sixty": tooDeep,
	} {
		t.Run(name, func(t *testing.T) {
			if matchesShard(identity, 123, expected) {
				t.Fatal("different shard identity accepted")
			}
		})
	}
	if matchesShard(valid, 122, expected) {
		t.Fatal("different state or block sequence number accepted")
	}
}

func TestInvalidBatchLimitsAreRejectedBeforeNetworkAccess(t *testing.T) {
	c := &Client{}
	for name, bodies := range map[string][][]byte{
		"empty":           nil,
		"too_many":        make([][]byte, 1025),
		"empty_body":      {{1}, {}},
		"oversized_body":  {make([]byte, (8<<20)+1)},
		"oversized_total": {make([]byte, 8<<20), {1}},
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := c.Send(context.Background(), bodies); err == nil {
				t.Fatal("invalid batch accepted")
			}
		})
	}
}

func TestInvalidAccountInputsAreRejectedBeforeNetworkAccess(t *testing.T) {
	c := &Client{}
	anchor := &ton.BlockIDExt{}
	basechain := address.NewAddress(0, 0, make([]byte, 32))
	masterchain := address.NewAddress(0, 255, make([]byte, 32))
	short := address.NewAddress(0, 0, make([]byte, 8))
	for name, addr := range map[string]*address.Address{"nil": nil, "masterchain": masterchain, "short": short} {
		t.Run(name, func(t *testing.T) {
			if _, err := c.Account(context.Background(), anchor, anchor, addr); err == nil {
				t.Fatal("invalid address accepted")
			}
		})
	}
	if _, err := c.Account(context.Background(), nil, anchor, basechain); err == nil {
		t.Fatal("missing masterchain anchor accepted")
	}
	if _, err := c.Account(context.Background(), anchor, nil, basechain); err == nil {
		t.Fatal("missing shard anchor accepted")
	}
}
