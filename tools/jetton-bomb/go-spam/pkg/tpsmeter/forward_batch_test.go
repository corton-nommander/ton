package tpsmeter

import (
	"context"
	"fmt"
	"sync"
	"testing"
	"time"
)

// gatedSource makes a catch-up observable: every Shards call parks until `want` of them are in flight at
// once, so a batched plan sails through while a serial walk can only ever reach one and times out.
type gatedSource struct {
	*modeledSource

	mu       sync.Mutex
	inflight int
	peak     int
	want     int
	armed    bool
	blocks   map[BlockID]int

	gate chan struct{}
	once sync.Once
}

func newGatedSource(inner *modeledSource) *gatedSource {
	return &gatedSource{modeledSource: inner, gate: make(chan struct{}), blocks: map[BlockID]int{}}
}

// arm starts requiring `want` concurrent Shards calls. Called after the anchor poll, which legitimately
// resolves a single masterchain block.
func (s *gatedSource) arm(want int) {
	s.mu.Lock()
	s.want = want
	s.armed = true
	s.mu.Unlock()
}

func (s *gatedSource) Shards(ctx context.Context, mc uint32) ([]BlockID, error) {
	s.mu.Lock()
	armed := s.armed
	if armed {
		s.inflight++
		if s.inflight > s.peak {
			s.peak = s.inflight
		}
		reached := s.peak >= s.want
		s.mu.Unlock()
		if reached {
			s.once.Do(func() { close(s.gate) })
		}
		select {
		case <-s.gate:
		case <-time.After(2 * time.Second):
		}
		s.mu.Lock()
		s.inflight--
	}
	s.mu.Unlock()
	return s.modeledSource.Shards(ctx, mc)
}

func (s *gatedSource) BlockInfo(ctx context.Context, id BlockID) (BlockInfo, error) {
	s.mu.Lock()
	s.blocks[id]++
	s.mu.Unlock()
	return s.modeledSource.BlockInfo(ctx, id)
}

func (s *gatedSource) stats() (peak int, repeated []BlockID) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for id, n := range s.blocks {
		if n > 1 {
			repeated = append(repeated, id)
		}
	}
	return s.peak, repeated
}

// A catch-up must resolve its masterchain steps in ONE parallel wave. The serial walk this replaced cost
// a full round-trip chain per masterchain block, which on a 4-shard testnet ran below the chain's own
// block rate — once the meter fell behind it never caught up, and every TPS window silently reported
// stale data (the windows are anchored at the newest INGESTED block, not at wall clock).
func TestForwardCatchupResolvesStepsInOneWave(t *testing.T) {
	source := newGatedSource(newLinearSource(8, 1_700_000_000, 3))
	source.setLatest(1)
	meter := New(source, Options{Windows: []int{60}, Workers: 8})
	meter.pollOnce(context.Background())

	source.arm(7) // masterchain 2..8
	source.setLatest(8)
	meter.pollOnce(context.Background())

	if meter.lastMC != 8 {
		t.Fatalf("catch-up stopped at MC %d, want 8", meter.lastMC)
	}
	peak, repeated := source.stats()
	if peak < 7 {
		t.Fatalf("peak concurrent Shards calls = %d, want 7 — the catch-up is walking serially again", peak)
	}
	if len(repeated) != 0 {
		t.Fatalf("blocks fetched more than once: %v", repeated)
	}
}

// injectedFailureSource fails specific blocks or masterchain shard lookups on demand.
type injectedFailureSource struct {
	*modeledSource

	mu         sync.Mutex
	failBlocks map[BlockID]bool
	failShards map[uint32]bool
}

func newInjectedFailureSource(inner *modeledSource) *injectedFailureSource {
	return &injectedFailureSource{
		modeledSource: inner,
		failBlocks:    map[BlockID]bool{},
		failShards:    map[uint32]bool{},
	}
}

func (s *injectedFailureSource) failBlock(id BlockID, fail bool) {
	s.mu.Lock()
	s.failBlocks[id] = fail
	s.mu.Unlock()
}

func (s *injectedFailureSource) failShardsAt(mc uint32, fail bool) {
	s.mu.Lock()
	s.failShards[mc] = fail
	s.mu.Unlock()
}

func (s *injectedFailureSource) Shards(ctx context.Context, mc uint32) ([]BlockID, error) {
	s.mu.Lock()
	bad := s.failShards[mc]
	s.mu.Unlock()
	if bad {
		return nil, fmt.Errorf("injected shards failure at MC %d", mc)
	}
	return s.modeledSource.Shards(ctx, mc)
}

func (s *injectedFailureSource) BlockInfo(ctx context.Context, id BlockID) (BlockInfo, error) {
	s.mu.Lock()
	bad := s.failBlocks[id]
	s.mu.Unlock()
	if bad {
		return BlockInfo{}, fmt.Errorf("injected block failure %+v", id)
	}
	return s.modeledSource.BlockInfo(ctx, id)
}

// One unreadable block inside a batch must cost only its own step onward: everything before it commits,
// and the checkpoint stops immediately before it so the next poll resumes there. Fetching the batch as
// one wave must not turn a single bad block into "the whole catch-up made no progress".
func TestForwardBatchCommitsPrefixBeforeAFailedBlock(t *testing.T) {
	inner := newLinearSource(6, 1_700_000_000, 3)
	source := newInjectedFailureSource(inner)
	source.setLatest(1)
	meter := New(source, Options{Windows: []int{60}, Workers: 8})
	meter.pollOnce(context.Background())
	if meter.lastMC != 1 {
		t.Fatalf("anchor poll left MC %d, want 1", meter.lastMC)
	}

	// Shard block 11 belongs to the MC 4 transition (stride 3 => MC 4 covers 10..12).
	broken := BlockID{Workchain: 0, Shard: testRootShard, Seqno: 11}
	source.failBlock(broken, true)
	source.setLatest(6)
	meter.pollOnce(context.Background())
	if meter.lastMC != 3 {
		t.Fatalf("checkpoint = MC %d, want 3 (commit up to the block before the failure)", meter.lastMC)
	}
	if !meter.Snapshot().PendingBatch {
		t.Fatal("a failed step must leave the forward pass pending")
	}

	source.failBlock(broken, false)
	meter.pollOnce(context.Background())
	if meter.lastMC != 6 {
		t.Fatalf("checkpoint after recovery = MC %d, want 6", meter.lastMC)
	}
	if meter.Snapshot().SkippedMC != 0 || meter.Snapshot().SkippedShards != 0 {
		t.Fatalf("recovery skipped blocks: %+v", meter.Snapshot())
	}
	// From the anchor's shard top (3) up: everything below it predates the anchor and belongs to backfill.
	for seqno := uint32(3); seqno <= 18; seqno++ {
		if !meter.seen[BlockID{Workchain: 0, Shard: testRootShard, Seqno: seqno}] {
			t.Fatalf("shard block %d missing after recovery", seqno)
		}
	}
}

// Same contract one phase earlier: if a step's shard tops cannot be resolved, the plan is truncated
// there and the steps before it still commit.
func TestForwardBatchTruncatesPlanAtAnUnresolvableStep(t *testing.T) {
	inner := newLinearSource(6, 1_700_000_000, 3)
	source := newInjectedFailureSource(inner)
	source.setLatest(1)
	meter := New(source, Options{Windows: []int{60}, Workers: 8})
	meter.pollOnce(context.Background())

	source.failShardsAt(4, true)
	source.setLatest(6)
	meter.pollOnce(context.Background())
	if meter.lastMC != 3 {
		t.Fatalf("checkpoint = MC %d, want 3 (plan truncated at the unresolvable step)", meter.lastMC)
	}

	source.failShardsAt(4, false)
	meter.pollOnce(context.Background())
	if meter.lastMC != 6 {
		t.Fatalf("checkpoint after recovery = MC %d, want 6", meter.lastMC)
	}
}
