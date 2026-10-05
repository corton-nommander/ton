// Package tpsmeter is a source-agnostic TON transactions-per-second meter,
// ported from the Python reference (tpsmeter.py). It walks masterchain blocks
// and the basechain shard blocks they reference, counts transactions, and
// exposes sliding-window TPS plus the live shard count.
//
// The measurement engine (Meter) is deliberately decoupled from the RPC layer
// (Source) so it can be reused two ways:
//
//   - cmd/tpsmeter: a live dashboard/stdout monitor (this repo's Go port of the
//     Python meter) — shard count + TPS at 1/5/15 min.
//   - cmd/spam (next step): a deterministic "control measurement". Run the meter
//     alongside the load for 15 min, then read TPS(600) — the average TPS over
//     the last 10 minutes — so the first 5 minutes absorb shard split + warm-up.
//     TPS(windowSec) at the stop moment == mean TPS over the last windowSec, so
//     the same sliding-window primitive serves both use cases.
package tpsmeter

import (
	"context"
	"fmt"
)

// Masterchain pseudo-shard identity (workchain -1, shard 0x8000000000000000).
const (
	mcWorkchain int32 = -1
	mcShard     int64 = -9223372036854775808 // 0x8000000000000000 as signed int64
)

// BlockID identifies a single block: (workchain, shard, seqno). Shard is the
// signed 64-bit shard prefix id as TON/toncenter report it.
type BlockID struct {
	Workchain int32
	Shard     int64
	Seqno     uint32
}

// BlockIdentity binds a block coordinate to its canonical TON block hashes.
type BlockIdentity struct {
	RootHash [32]byte
	FileHash [32]byte
}

func blockIdentity(rootHash, fileHash []byte) (BlockIdentity, error) {
	if len(rootHash) != 32 || len(fileHash) != 32 {
		return BlockIdentity{}, fmt.Errorf("block identity hashes must both be 32 bytes")
	}
	var identity BlockIdentity
	copy(identity.RootHash[:], rootHash)
	copy(identity.FileHash[:], fileHash)
	return identity, nil
}

func (identity BlockIdentity) isZero() bool { return identity == (BlockIdentity{}) }

// BlockInfo is the per-block data the meter needs: when it was generated and how
// many transactions it holds. Partial is true when the tx count is a lower bound
// (the source hit its pagination cap).
type BlockInfo struct {
	GenUtime int64
	TxCount  int
	Partial  bool
}

// Source is the block-walking RPC surface the Meter drives. Implementations are
// free to back it with toncenter v2 HTTP (see NewToncenterSource), a liteserver,
// or a fake for tests.
type Source interface {
	// LatestMasterchainSeqno returns the current top masterchain block seqno.
	LatestMasterchainSeqno(ctx context.Context) (uint32, error)
	// MasterchainIdentity resolves the canonical hashes at one MC seqno.
	MasterchainIdentity(ctx context.Context, seqno uint32) (BlockIdentity, error)
	// Shards returns the shard-block tops referenced by a given MC block. On the
	// basechain these are the workchain-0 shards; their count is the shard count.
	Shards(ctx context.Context, mcSeqno uint32) ([]BlockID, error)
	// BlockUtime reads only the block header, without enumerating its transactions.
	BlockUtime(ctx context.Context, id BlockID) (int64, error)
	// BlockInfo returns gen_utime + transaction count for one block.
	BlockInfo(ctx context.Context, id BlockID) (BlockInfo, error)
	// Description is a short human label for status output.
	Description() string
}
