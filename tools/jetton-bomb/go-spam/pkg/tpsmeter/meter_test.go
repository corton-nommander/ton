package tpsmeter

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"reflect"
	"sync"
	"testing"
)

func TestTPSWindow(t *testing.T) {
	meter := New(nopSource{}, Options{Windows: []int{10, 60}})
	for utime := int64(100); utime <= 110; utime++ {
		if err := meter.appendStat(Stat{GenUtime: utime, TxCount: 5}); err != nil {
			t.Fatal(err)
		}
	}
	w10 := meter.TPS(10)
	if w10.Blocks != 10 || w10.TotalTx != 50 || w10.TPS != 5 || w10.Filling {
		t.Fatalf("window 10: %+v", w10)
	}
	w60 := meter.TPS(60)
	if w60.Blocks != 11 || w60.TotalTx != 55 || w60.TPS != 55.0/60.0 || !w60.Filling {
		t.Fatalf("window 60: %+v", w60)
	}
}

func TestWindowBlockRateSplitsMasterchainFromShards(t *testing.T) {
	meter := New(nopSource{}, Options{Windows: []int{60}})
	// 60 seconds: one masterchain block per second, two shard blocks per second.
	for utime := int64(100); utime < 160; utime++ {
		if err := meter.appendStat(Stat{GenUtime: utime, TxCount: 1, IsMC: true}); err != nil {
			t.Fatal(err)
		}
		for shard := 0; shard < 2; shard++ {
			if err := meter.appendStat(Stat{GenUtime: utime, TxCount: 3}); err != nil {
				t.Fatal(err)
			}
		}
	}
	// The window is anchored at the newest gen_utime (159) and keeps everything
	// strictly after 159-60, so all 60 seeded seconds are in range.
	window := meter.TPS(60)
	if window.MCBlocks != 60 || window.WCBlocks != 120 || window.Blocks != 180 {
		t.Fatalf("window counts: %+v", window)
	}
	if window.MCBlockRate != 1 || window.WCBlockRate != 2 || window.BlockRate != 3 {
		t.Fatalf("window rates: %+v", window)
	}
	snapshot := meter.Snapshot()
	if snapshot.MCBlocks != 60 || snapshot.WCBlocks != 120 || snapshot.Blocks != 180 {
		t.Fatalf("snapshot counts: %+v", snapshot)
	}
}

func TestEviction(t *testing.T) {
	meter := New(nopSource{}, Options{Windows: []int{10}, HistoryMax: 5})
	for utime := int64(0); utime < 100; utime++ {
		if err := meter.appendStat(Stat{GenUtime: utime, TxCount: 1}); err != nil {
			t.Fatal(err)
		}
	}
	if got := meter.Snapshot().Blocks; got != 5 {
		t.Fatalf("history len = %d, want 5", got)
	}
	window := meter.TPS(10)
	if window.Blocks != 5 || window.TotalTx != 5 {
		t.Fatalf("after eviction: %+v", window)
	}
}

func TestFailedBatchWritesNothingBeforeCheckpoint(t *testing.T) {
	path := t.TempDir() + "/poll.db"
	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	source := &flakySource{}
	meter := New(source, Options{Windows: []int{60}, Workers: 2, Recorder: store})
	meter.pollOnce(context.Background())
	state, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if state.Checkpoint.HaveMC || state.Status.Blocks != 0 || state.Status.CoverageValid {
		t.Fatalf("failed fetch changed durable state: %+v", state)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	store, err = OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	meter = New(source, Options{Windows: []int{60}, Workers: 2, Recorder: store})
	meter.pollOnce(context.Background())
	state, err = store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if !state.Checkpoint.HaveMC || state.Checkpoint.MCSeqno != 42 || state.Status.Blocks != 2 {
		t.Fatalf("retry did not atomically commit: %+v", state)
	}
}

func TestPersistentCatchupIsOldestFirstBeyondCaps(t *testing.T) {
	path := t.TempDir() + "/catchup.db"
	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	source := newLinearSource(8, 1_700_000_000, 3)
	source.setLatest(1)
	meter := New(source, Options{Recorder: store, MCMaxCatchup: 2, MaxCatchup: 1, Workers: 8})
	meter.pollOnce(context.Background())
	source.setLatest(8)
	for range 4 {
		meter.pollOnce(context.Background())
	}
	state, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if state.Checkpoint.MCSeqno != 8 || !state.Status.CoverageValid {
		t.Fatalf("catch-up state: %+v", state)
	}
	if meter.Snapshot().SkippedMC != 0 || meter.Snapshot().SkippedShards != 0 {
		t.Fatalf("persistent catch-up reported skips: %+v", meter.Snapshot())
	}
	for seqno := uint32(1); seqno <= 8; seqno++ {
		if _, found, err := store.Block(BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: seqno}); err != nil || !found {
			t.Fatalf("MC %d missing after catch-up: found=%v err=%v", seqno, found, err)
		}
	}
	for seqno := uint32(3); seqno <= 24; seqno++ {
		if _, found, err := store.Block(BlockID{Workchain: 0, Shard: testRootShard, Seqno: seqno}); err != nil || !found {
			t.Fatalf("shard block %d missing after catch-up: found=%v err=%v", seqno, found, err)
		}
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	reopened, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if reopened.Checkpoint.MCSeqno != 8 || !reopened.Status.CoverageValid {
		t.Fatalf("catch-up did not survive reopen: %+v", reopened)
	}
}

func TestForwardSplitMergeTraversal(t *testing.T) {
	source, expected := newSplitMergeSource()
	store, err := OpenStore(t.TempDir() + "/topology-forward.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	source.setLatest(1)
	meter := New(source, Options{Recorder: store, Workers: 8})
	meter.pollOnce(context.Background())
	source.setLatest(4)
	meter.pollOnce(context.Background())
	assertTopologyStore(t, store, expected)
	state, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if state.Checkpoint.MCSeqno != 4 || len(state.Checkpoint.Shards) != 1 || !state.Status.CoverageValid {
		t.Fatalf("forward topology state: %+v", state)
	}
}

func TestBackfillSplitMergeTraversal(t *testing.T) {
	source, expected := newSplitMergeSource()
	store, err := OpenStore(t.TempDir() + "/topology-backfill.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	source.setLatest(4)
	meter := New(source, Options{Recorder: store, Workers: 8, Windows: []int{30}, Backfill: true})
	meter.pollOnce(context.Background())
	meter.backfill(context.Background())
	assertTopologyStore(t, store, expected)
	state, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if state.Coverage.LowMC != 1 || state.Coverage.HighMC != 4 || !state.Status.CoverageValid || !meter.Snapshot().BackfillComplete {
		t.Fatalf("backfill topology state: store=%+v meter=%+v", state, meter.Snapshot())
	}
}

func TestBackfillTreatsShardSeqnoZeroAsZerostate(t *testing.T) {
	base := int64(1_700_250_000)
	root := BlockID{Workchain: 0, Shard: testRootShard}
	source := &modeledSource{
		latest: 2,
		shards: map[uint32][]BlockID{
			1: {{Workchain: root.Workchain, Shard: root.Shard, Seqno: 0}},
			2: {{Workchain: root.Workchain, Shard: root.Shard, Seqno: 1}},
		},
		infos: map[BlockID]BlockInfo{
			{Workchain: mcWorkchain, Shard: mcShard, Seqno: 1}:       {GenUtime: base},
			{Workchain: mcWorkchain, Shard: mcShard, Seqno: 2}:       {GenUtime: base + 2},
			{Workchain: root.Workchain, Shard: root.Shard, Seqno: 1}: {GenUtime: base + 1},
		},
	}
	store, err := OpenStore(t.TempDir() + "/zerostate-backfill.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	meter := New(source, Options{Recorder: store, Windows: []int{30}, Backfill: true})
	meter.pollOnce(context.Background())
	meter.backfill(context.Background())
	state, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if state.Coverage.LowMC != 1 || !meter.Snapshot().BackfillComplete || meter.Snapshot().Errors != 0 {
		t.Fatalf("zerostate backfill did not complete: store=%+v meter=%+v", state, meter.Snapshot())
	}
	if _, found, err := store.Block(BlockID{Workchain: root.Workchain, Shard: root.Shard, Seqno: 0}); err != nil || found {
		t.Fatalf("shard zerostate was recorded as a block: found=%v err=%v", found, err)
	}
}

func TestLowerChainTopInvalidatesDurableCoverage(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/rollback.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	source := newLinearSource(2, 1_700_100_000, 1)
	meter := New(source, Options{Recorder: store})
	meter.pollOnce(context.Background())
	source.setLatest(3)
	meter.pollOnce(context.Background())
	source.setLatest(1)
	meter.pollOnce(context.Background())
	status, err := store.Status()
	if err != nil {
		t.Fatal(err)
	}
	if status.CoverageValid || !status.RollbackDetected || !meter.Snapshot().RollbackDetected {
		t.Fatalf("rollback was not detected: status=%+v snapshot=%+v", status, meter.Snapshot())
	}
}

func TestCheckpointIdentityMismatchAfterReopen(t *testing.T) {
	for _, liveTip := range []uint32{2, 3} {
		t.Run(fmt.Sprintf("tip-%d", liveTip), func(t *testing.T) {
			path := t.TempDir() + "/identity.db"
			store, err := OpenStore(path)
			if err != nil {
				t.Fatal(err)
			}
			seedSource := newLinearSource(3, 1_700_300_000, 1)
			seedSource.setLatest(1)
			meter := New(seedSource, Options{Recorder: store})
			meter.pollOnce(context.Background())
			seedSource.setLatest(2)
			meter.pollOnce(context.Background())
			before, err := store.LoadState()
			if err != nil {
				t.Fatal(err)
			}
			if before.Checkpoint.MCIdentity != testIdentity(2) {
				t.Fatalf("stored checkpoint identity = %+v", before.Checkpoint.MCIdentity)
			}
			if err := store.Close(); err != nil {
				t.Fatal(err)
			}

			store, err = OpenStore(path)
			if err != nil {
				t.Fatal(err)
			}
			mismatchSource := newLinearSource(3, 1_700_300_000, 1)
			mismatchSource.setLatest(liveTip)
			mismatch := testIdentity(2)
			mismatch.RootHash[0] ^= 0xff
			mismatchSource.identities = map[uint32]BlockIdentity{2: mismatch}
			meter = New(mismatchSource, Options{Recorder: store})
			meter.pollOnce(context.Background())
			state, err := store.LoadState()
			if err != nil {
				t.Fatal(err)
			}
			if state.Status.CoverageValid || !state.Status.RollbackDetected || state.Checkpoint.MCSeqno != 2 || state.Status.Blocks != before.Status.Blocks {
				t.Fatalf("identity mismatch state: before=%+v after=%+v", before, state)
			}
			if meter.Snapshot().SourceVerified {
				t.Fatal("mismatched checkpoint was marked source-verified")
			}
			if err := store.Close(); err != nil {
				t.Fatal(err)
			}
			store, err = OpenStore(path)
			if err != nil {
				t.Fatal(err)
			}
			defer store.Close()
			reopened, err := store.Status()
			if err != nil {
				t.Fatal(err)
			}
			if reopened.CoverageValid || !reopened.RollbackDetected {
				t.Fatalf("identity mismatch was not durable: %+v", reopened)
			}
		})
	}
}

func TestEmptyShardTopsDoNotCommitCoverage(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/empty-shards.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	source := &modeledSource{
		latest: 1,
		shards: map[uint32][]BlockID{1: {}},
		infos:  map[BlockID]BlockInfo{},
	}
	meter := New(source, Options{Recorder: store})
	meter.pollOnce(context.Background())
	state, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if state.Checkpoint.HaveMC || state.Status.Blocks != 0 || state.Status.CoverageValid || !meter.Snapshot().PendingBatch {
		t.Fatalf("empty shards advanced state: store=%+v meter=%+v", state, meter.Snapshot())
	}
}

func TestBackfillSuccessDoesNotClearForwardPending(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/pending.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	shard := shardKey{wc: 0, sh: testRootShard}
	anchor := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 3}, GenUtime: 106, IsMC: true},
		{ID: blockID(shard, 3), GenUtime: 105},
	}
	if _, err := store.CommitForward(anchor, testCheckpoint(3, map[shardKey]uint32{shard: 3}), 106, true); err != nil {
		t.Fatal(err)
	}
	source := &modeledSource{latest: 4, shards: map[uint32][]BlockID{}, infos: map[BlockID]BlockInfo{}}
	meter := New(source, Options{Recorder: store})
	meter.pollOnce(context.Background())
	if !meter.Snapshot().PendingBatch {
		t.Fatal("failed forward poll did not set pending state")
	}
	older := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 2}, GenUtime: 102, IsMC: true},
		{ID: blockID(shard, 2), GenUtime: 101},
	}
	if err := meter.commitBackfill(older, testCheckpoint(2, map[shardKey]uint32{shard: 2}), 102, true); err != nil {
		t.Fatal(err)
	}
	if !meter.Snapshot().PendingBatch || meter.Snapshot().ForwardSynced {
		t.Fatalf("backfill cleared forward pending state: %+v", meter.Snapshot())
	}
	handler := NewHTTPHandler(meter, store)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("pending forward health = %d, want 503", response.Code)
	}
	source.setLatest(3)
	meter.pollOnce(context.Background())
	if meter.Snapshot().PendingBatch || !meter.Snapshot().ForwardSynced {
		t.Fatalf("successful forward synchronization did not clear pending: %+v", meter.Snapshot())
	}
}

type nopSource struct{}

func (nopSource) LatestMasterchainSeqno(context.Context) (uint32, error) { return 0, nil }
func (nopSource) MasterchainIdentity(context.Context, uint32) (BlockIdentity, error) {
	return testIdentity(0), nil
}
func (nopSource) Shards(context.Context, uint32) ([]BlockID, error)     { return nil, nil }
func (nopSource) BlockUtime(context.Context, BlockID) (int64, error)    { return 0, nil }
func (nopSource) BlockInfo(context.Context, BlockID) (BlockInfo, error) { return BlockInfo{}, nil }
func (nopSource) Description() string                                   { return "nop" }

type flakySource struct {
	mu     sync.Mutex
	failed bool
}

func (*flakySource) LatestMasterchainSeqno(context.Context) (uint32, error) { return 42, nil }
func (*flakySource) MasterchainIdentity(_ context.Context, seqno uint32) (BlockIdentity, error) {
	return testIdentity(seqno), nil
}
func (*flakySource) Shards(context.Context, uint32) ([]BlockID, error) {
	return []BlockID{{Workchain: 0, Shard: testRootShard, Seqno: 100}}, nil
}
func (*flakySource) BlockUtime(_ context.Context, id BlockID) (int64, error) {
	return 1_700_000_000 + int64(id.Seqno), nil
}
func (source *flakySource) BlockInfo(_ context.Context, id BlockID) (BlockInfo, error) {
	if id.Workchain != mcWorkchain {
		source.mu.Lock()
		defer source.mu.Unlock()
		if !source.failed {
			source.failed = true
			return BlockInfo{}, errors.New("temporary block failure")
		}
	}
	return BlockInfo{GenUtime: 1_700_000_000 + int64(id.Seqno), TxCount: 1}, nil
}
func (*flakySource) Description() string { return "flaky" }

type modeledSource struct {
	mu         sync.Mutex
	latest     uint32
	shards     map[uint32][]BlockID
	infos      map[BlockID]BlockInfo
	identities map[uint32]BlockIdentity
}

func (source *modeledSource) setLatest(latest uint32) {
	source.mu.Lock()
	source.latest = latest
	source.mu.Unlock()
}

func (source *modeledSource) LatestMasterchainSeqno(context.Context) (uint32, error) {
	source.mu.Lock()
	defer source.mu.Unlock()
	return source.latest, nil
}

func (source *modeledSource) MasterchainIdentity(_ context.Context, seqno uint32) (BlockIdentity, error) {
	if identity, ok := source.identities[seqno]; ok {
		return identity, nil
	}
	return testIdentity(seqno), nil
}

func (source *modeledSource) Shards(_ context.Context, mc uint32) ([]BlockID, error) {
	shards, ok := source.shards[mc]
	if !ok {
		return nil, fmt.Errorf("unknown MC %d", mc)
	}
	return append([]BlockID(nil), shards...), nil
}

func (source *modeledSource) BlockInfo(_ context.Context, id BlockID) (BlockInfo, error) {
	info, ok := source.infos[id]
	if !ok {
		return BlockInfo{}, fmt.Errorf("unknown block %+v", id)
	}
	return info, nil
}

func (source *modeledSource) BlockUtime(ctx context.Context, id BlockID) (int64, error) {
	info, err := source.BlockInfo(ctx, id)
	return info.GenUtime, err
}

func (*modeledSource) Description() string { return "modeled" }

func newLinearSource(latest uint32, base int64, shardStride uint32) *modeledSource {
	source := &modeledSource{latest: latest, shards: map[uint32][]BlockID{}, infos: map[BlockID]BlockInfo{}}
	for mc := uint32(1); mc <= latest; mc++ {
		shardTop := mc * shardStride
		source.shards[mc] = []BlockID{{Workchain: 0, Shard: testRootShard, Seqno: shardTop}}
		source.infos[BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: mc}] = BlockInfo{GenUtime: base + int64(mc)*2, TxCount: 1}
		for seqno := (mc-1)*shardStride + 1; seqno <= shardTop; seqno++ {
			source.infos[BlockID{Workchain: 0, Shard: testRootShard, Seqno: seqno}] = BlockInfo{GenUtime: base + int64(mc)*2 - 1, TxCount: 1}
		}
	}
	return source
}

func newSplitMergeSource() (*modeledSource, map[BlockID]bool) {
	base := int64(1_700_200_000)
	root := shardKey{wc: 0, sh: testRootShard}
	left, right, ok := childShardKeys(root)
	if !ok {
		panic("root shard has no children")
	}
	source := &modeledSource{latest: 4, shards: map[uint32][]BlockID{}, infos: map[BlockID]BlockInfo{}}
	source.shards[1] = []BlockID{blockID(root, 100)}
	source.shards[2] = []BlockID{blockID(left, 102), blockID(right, 102)}
	source.shards[3] = []BlockID{blockID(left, 103), blockID(right, 103)}
	source.shards[4] = []BlockID{blockID(root, 105)}
	expected := map[BlockID]bool{}
	for mc := uint32(1); mc <= 4; mc++ {
		id := BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: mc}
		source.infos[id] = BlockInfo{GenUtime: base + int64(mc)*10}
		expected[id] = true
	}
	shardBlocks := map[BlockID]int64{
		blockID(root, 100):  base + 9,
		blockID(left, 101):  base + 19,
		blockID(left, 102):  base + 19,
		blockID(left, 103):  base + 29,
		blockID(right, 101): base + 19,
		blockID(right, 102): base + 19,
		blockID(right, 103): base + 29,
		blockID(root, 104):  base + 39,
		blockID(root, 105):  base + 39,
	}
	for id, utime := range shardBlocks {
		source.infos[id] = BlockInfo{GenUtime: utime, TxCount: 1}
		expected[id] = true
	}
	return source, expected
}

func assertTopologyStore(t *testing.T, store *Store, expected map[BlockID]bool) {
	t.Helper()
	status, err := store.Status()
	if err != nil {
		t.Fatal(err)
	}
	if status.Blocks != uint64(len(expected)) {
		t.Fatalf("stored blocks = %d, want %d", status.Blocks, len(expected))
	}
	for id := range expected {
		if _, found, err := store.Block(id); err != nil || !found {
			t.Fatalf("expected block %+v: found=%v err=%v", id, found, err)
		}
	}
}

// Mainnet merges siblings from unequal seqnos (the merged block's parents are
// each sibling's own last block), so that must stay a lineage-valid transition.
func TestMergeFromUnequalSiblingSeqnosIsValid(t *testing.T) {
	root := shardKey{wc: 0, sh: testRootShard}
	left, right, ok := childShardKeys(root)
	if !ok {
		t.Fatal("root shard has no children")
	}
	previous := map[shardKey]uint32{left: 130, right: 99}
	current := map[shardKey]uint32{root: 132}

	targets, valid := transitionTargets(previous, current)
	if !valid {
		t.Fatal("merge from unequal sibling seqnos must be lineage-valid")
	}
	got := map[BlockID]bool{}
	for _, target := range targets {
		got[target.id] = true
	}
	want := map[BlockID]bool{blockID(root, 131): true, blockID(root, 132): true}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("merge targets: got %v want %v", got, want)
	}

	// A merged top at or below the highest parent is still a broken transition.
	if _, valid := transitionTargets(previous, map[shardKey]uint32{root: 130}); valid {
		t.Fatal("merged top below the highest parent must be invalid")
	}
}
