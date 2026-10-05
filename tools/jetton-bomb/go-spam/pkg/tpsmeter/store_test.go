package tpsmeter

import (
	"reflect"
	"testing"
	"time"

	bolt "go.etcd.io/bbolt"
)

const testRootShard = int64(-9223372036854775808)

func testIdentity(seqno uint32) BlockIdentity {
	var identity BlockIdentity
	identity.RootHash[0] = byte(seqno)
	identity.RootHash[31] = byte(seqno >> 8)
	identity.FileHash[0] = byte(seqno ^ 0xa5)
	identity.FileHash[31] = byte((seqno >> 8) ^ 0x5a)
	return identity
}

func testCheckpoint(mc uint32, shards map[shardKey]uint32) Checkpoint {
	return checkpointFor(mc, shards, testIdentity(mc))
}

func TestStoreAtomicCommitDedupeAndReopen(t *testing.T) {
	path := t.TempDir() + "/test.db"
	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}

	direct := Stat{ID: BlockID{Workchain: 1, Shard: 2, Seqno: 7}, GenUtime: 50, TxCount: 5}
	inserted, err := store.RecordBlock(direct)
	if err != nil || !inserted {
		t.Fatalf("first record: inserted=%v err=%v", inserted, err)
	}
	duplicate := direct
	duplicate.GenUtime++
	duplicate.TxCount = 99
	inserted, err = store.RecordBlock(duplicate)
	if err != nil || inserted {
		t.Fatalf("duplicate record: inserted=%v err=%v", inserted, err)
	}

	anchorCP := testCheckpoint(42, map[shardKey]uint32{{wc: 0, sh: testRootShard}: 100})
	anchor := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 42}, GenUtime: 100, IsMC: true},
		{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: 100}, GenUtime: 99},
	}
	if _, err := store.CommitForward(anchor, anchorCP, 100, true); err != nil {
		t.Fatal(err)
	}
	nextCP := testCheckpoint(43, map[shardKey]uint32{{wc: 0, sh: testRootShard}: 102})
	next := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 43}, GenUtime: 103, IsMC: true},
		{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: 101}, GenUtime: 101, TxCount: 7},
		{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: 102}, GenUtime: 102},
	}
	if _, err := store.CommitForward(next, nextCP, 103, true); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	store, err = OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	gotDirect, found, err := store.Block(direct.ID)
	if err != nil || !found || !reflect.DeepEqual(gotDirect, direct) {
		t.Fatalf("reopen direct block: found=%v got=%+v err=%v", found, gotDirect, err)
	}
	state, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(state.Checkpoint, nextCP) {
		t.Fatalf("checkpoint: got %+v want %+v", state.Checkpoint, nextCP)
	}
	if state.Status.Epoch != 1 || !state.Status.CoverageValid || state.Status.CoverageStart.Unix() != 101 || state.Status.CoverageEnd.Unix() != 102 {
		t.Fatalf("coverage after reopen: %+v", state.Status)
	}
	summary, err := store.Summary(time.Unix(101, 0), time.Unix(102, 0), 1)
	if err != nil {
		t.Fatal(err)
	}
	if !summary.Complete || summary.RunAverage != 7 {
		t.Fatalf("covered summary: %+v", summary)
	}
}

func TestStoreMigratesV3IntoEpochOne(t *testing.T) {
	path := t.TempDir() + "/v3.db"
	id := BlockID{Workchain: 0, Shard: testRootShard, Seqno: 7}
	stat := Stat{ID: id, GenUtime: 101, TxCount: 7}
	cp := Checkpoint{HaveMC: true, MCSeqno: 2, MCIdentity: testIdentity(2), Shards: map[ShardID]uint32{}}
	coverage := CoverageState{
		HaveAnchor: true,
		Valid:      true,
		LowMC:      1,
		HighMC:     2,
		LowUtime:   100,
		HighUtime:  102,
		LowShards:  map[ShardID]uint32{},
	}
	db, err := bolt.Open(path, 0o600, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := db.Update(func(tx *bolt.Tx) error {
		blocks, err := tx.CreateBucket(blocksBucket)
		if err != nil {
			return err
		}
		seconds, err := tx.CreateBucket(secondsBucket)
		if err != nil {
			return err
		}
		meta, err := tx.CreateBucket(metaBucket)
		if err != nil {
			return err
		}
		if err := blocks.Put(encodeBlockID(id), encodeStat(stat)); err != nil {
			return err
		}
		if err := seconds.Put(encodeSecond(stat.GenUtime), encodeAggregate(secondAggregate{WCTx: 7, WCBlocks: 1})); err != nil {
			return err
		}
		if err := putUint64(meta, schemaKey, 3); err != nil {
			return err
		}
		if err := putUint64(meta, blockCountKey, 1); err != nil {
			return err
		}
		if err := meta.Put(checkpointKey, encodeCheckpoint(cp)); err != nil {
			return err
		}
		return meta.Put(coverageKey, encodeCoverage(coverage))
	}); err != nil {
		t.Fatal(err)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}

	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	state, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if state.Status.Epoch != 1 || state.Status.Blocks != 1 || !state.Status.CoverageValid || !reflect.DeepEqual(state.Checkpoint, cp) {
		t.Fatalf("migrated state: %+v", state)
	}
	got, found, err := store.BlockAtEpoch(1, id)
	if err != nil || !found || got != stat {
		t.Fatalf("migrated block: found=%v got=%+v err=%v", found, got, err)
	}
	summary, err := store.Summary(time.Unix(101, 0), time.Unix(102, 0), 1)
	if err != nil {
		t.Fatal(err)
	}
	if !summary.Complete || summary.RunAverage != 7 || !reflect.DeepEqual(summary.Epochs, []uint64{1}) {
		t.Fatalf("migrated summary: %+v", summary)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	db, err = bolt.Open(path, 0o600, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if err := db.View(func(tx *bolt.Tx) error {
		version, err := readRequiredUint64(tx.Bucket(metaBucket), schemaKey)
		if err != nil {
			return err
		}
		active, err := readRequiredUint64(tx.Bucket(metaBucket), activeEpochKey)
		if err != nil {
			return err
		}
		if version != 4 || active != 1 || tx.Bucket(blocksBucket) != nil || tx.Bucket(secondsBucket) != nil {
			t.Fatalf("v4 root layout: schema=%d active=%d blocks=%v seconds=%v", version, active, tx.Bucket(blocksBucket), tx.Bucket(secondsBucket))
		}
		epoch, err := epochDataByID(tx, 1)
		if err != nil {
			return err
		}
		if epoch.blocks.Get(encodeBlockID(id)) == nil || epoch.seconds.Get(encodeSecond(stat.GenUtime)) == nil {
			t.Fatal("v3 records were not copied into epoch 1")
		}
		return nil
	}); err != nil {
		t.Fatal(err)
	}
}

func TestImportStorePreservesActiveEpochAndFutureRotation(t *testing.T) {
	id := BlockID{Workchain: 0, Shard: testRootShard, Seqno: 1}
	destination, err := OpenStore(t.TempDir() + "/destination.db")
	if err != nil {
		t.Fatal(err)
	}
	defer destination.Close()
	activeStat := Stat{ID: id, GenUtime: 200, TxCount: 5}
	if _, err := destination.RecordBlock(activeStat); err != nil {
		t.Fatal(err)
	}

	sourcePath := t.TempDir() + "/source.db"
	source, err := OpenStore(sourcePath)
	if err != nil {
		t.Fatal(err)
	}
	importedStat := Stat{ID: id, GenUtime: 100, TxCount: 3}
	if _, err := source.RecordBlock(importedStat); err != nil {
		t.Fatal(err)
	}
	if err := source.Close(); err != nil {
		t.Fatal(err)
	}

	imported, err := destination.ImportStore(sourcePath)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(imported, []uint64{2}) {
		t.Fatalf("imported epochs = %v", imported)
	}
	status, err := destination.Status()
	if err != nil {
		t.Fatal(err)
	}
	if status.Epoch != 1 || status.Blocks != 1 {
		t.Fatalf("import changed active epoch: %+v", status)
	}
	if got, found, err := destination.Block(id); err != nil || !found || got != activeStat {
		t.Fatalf("active block after import: found=%v got=%+v err=%v", found, got, err)
	}
	if got, found, err := destination.BlockAtEpoch(2, id); err != nil || !found || got != importedStat {
		t.Fatalf("imported block: found=%v got=%+v err=%v", found, got, err)
	}
	if err := destination.ResetLiveChain(); err != nil {
		t.Fatal(err)
	}
	status, err = destination.Status()
	if err != nil {
		t.Fatal(err)
	}
	if status.Epoch != 3 {
		t.Fatalf("reset reused imported epoch ID: %+v", status)
	}
}

func TestStandaloneBlocksNeverCreateCoverage(t *testing.T) {
	path := t.TempDir() + "/orphan.db"
	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	stats := []Stat{
		{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: 1}, GenUtime: 100, TxCount: 3},
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 1}, GenUtime: 101, TxCount: 2, IsMC: true},
	}
	for _, stat := range stats {
		if _, err := store.RecordBlock(stat); err != nil {
			t.Fatal(err)
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
	status, err := store.Status()
	if err != nil {
		t.Fatal(err)
	}
	if status.CoverageValid || status.CoverageStart != nil || status.CoverageEnd != nil {
		t.Fatalf("orphan/WC-only records established coverage: %+v", status)
	}
	summary, err := store.Summary(time.Unix(100, 0), time.Unix(102, 0), 1)
	if err != nil {
		t.Fatal(err)
	}
	if summary.Complete {
		t.Fatal("orphan blocks were reported complete after reopen")
	}
}

func TestRejectedBatchDoesNotWriteBlocksOrCheckpoint(t *testing.T) {
	path := t.TempDir() + "/failed.db"
	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	cp := testCheckpoint(10, map[shardKey]uint32{{wc: 0, sh: testRootShard}: 20})
	stats := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 10}, GenUtime: 100, IsMC: true},
		{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: 20}, GenUtime: 99, Partial: true},
	}
	if _, err := store.CommitForward(stats, cp, 100, true); err == nil {
		t.Fatal("partial canonical batch committed")
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	store, err = OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	state, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if state.Checkpoint.HaveMC || state.Status.Blocks != 0 || state.Status.CoverageValid {
		t.Fatalf("failed batch changed durable state: %+v", state)
	}
}

func TestForwardEqualWatermarkRemainsValidThenAdvances(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/forward-watermark.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	shard := shardKey{wc: 0, sh: testRootShard}
	anchorCP := testCheckpoint(1, map[shardKey]uint32{shard: 1})
	anchor := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 1}, GenUtime: 100, IsMC: true},
		{ID: blockID(shard, 1), GenUtime: 99},
	}
	if _, err := store.CommitForward(anchor, anchorCP, 100, true); err != nil {
		t.Fatal(err)
	}

	sameTopCP := testCheckpoint(2, map[shardKey]uint32{shard: 1})
	sameTop := []Stat{{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 2}, GenUtime: 101, IsMC: true}}
	state, err := store.CommitForward(sameTop, sameTopCP, 101, true)
	if err != nil {
		t.Fatal(err)
	}
	if !state.Coverage.Valid || state.Coverage.HighUtime != 99 {
		t.Fatalf("equal high watermark changed validity or bound: %+v", state.Coverage)
	}

	advancedCP := testCheckpoint(3, map[shardKey]uint32{shard: 2})
	advanced := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 3}, GenUtime: 103, IsMC: true},
		{ID: blockID(shard, 2), GenUtime: 102},
	}
	state, err = store.CommitForward(advanced, advancedCP, 103, true)
	if err != nil {
		t.Fatal(err)
	}
	if !state.Coverage.Valid || state.Coverage.HighUtime != 102 || !state.Status.CoverageValid {
		t.Fatalf("advanced high watermark: %+v", state)
	}
	if state.Status.CoverageStart.Unix() != 101 || state.Status.CoverageEnd.Unix() != 102 {
		t.Fatalf("forward coverage bounds: %+v", state.Status)
	}
}

func TestBackfillEqualWatermarkRemainsValidThenAdvances(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/backfill-watermark.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	shard := shardKey{wc: 0, sh: testRootShard}
	anchorCP := testCheckpoint(3, map[shardKey]uint32{shard: 2})
	anchor := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 3}, GenUtime: 104, IsMC: true},
		{ID: blockID(shard, 2), GenUtime: 103},
	}
	if _, err := store.CommitForward(anchor, anchorCP, 104, true); err != nil {
		t.Fatal(err)
	}

	sameTopCP := testCheckpoint(2, map[shardKey]uint32{shard: 2})
	sameTop := []Stat{{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 2}, GenUtime: 104, IsMC: true}}
	state, err := store.CommitBackfill(sameTop, sameTopCP, 104, true)
	if err != nil {
		t.Fatal(err)
	}
	if !state.Coverage.Valid || state.Coverage.LowUtime != 104 {
		t.Fatalf("equal low watermark changed validity or bound: %+v", state.Coverage)
	}

	advancedCP := testCheckpoint(1, map[shardKey]uint32{shard: 1})
	advanced := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 1}, GenUtime: 100, IsMC: true},
		{ID: blockID(shard, 1), GenUtime: 99},
	}
	state, err = store.CommitBackfill(advanced, advancedCP, 100, true)
	if err != nil {
		t.Fatal(err)
	}
	if !state.Coverage.Valid || state.Coverage.LowUtime != 100 || !state.Status.CoverageValid {
		t.Fatalf("advanced low watermark: %+v", state)
	}
	if state.Status.CoverageStart.Unix() != 101 || state.Status.CoverageEnd.Unix() != 103 {
		t.Fatalf("backfill coverage bounds: %+v", state.Status)
	}
}

func TestCanonicalBatchRejectsConflictingStoredBlock(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/conflict.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	shardID := BlockID{Workchain: 0, Shard: testRootShard, Seqno: 1}
	persisted := Stat{ID: shardID, GenUtime: 99, TxCount: 9}
	if _, err := store.RecordBlock(persisted); err != nil {
		t.Fatal(err)
	}
	cp := testCheckpoint(1, map[shardKey]uint32{{wc: 0, sh: testRootShard}: 1})
	batch := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 1}, GenUtime: 100, IsMC: true},
		{ID: shardID, GenUtime: 99, TxCount: 1},
	}
	if _, err := store.CommitForward(batch, cp, 100, true); err == nil {
		t.Fatal("conflicting persisted block was accepted by canonical batch")
	}
	state, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if state.Checkpoint.HaveMC || state.Status.Blocks != 1 || state.Status.CoverageValid {
		t.Fatalf("conflicting batch changed atomic state: %+v", state)
	}
	got, found, err := store.Block(shardID)
	if err != nil || !found || got != persisted {
		t.Fatalf("persisted conflict source changed: found=%v got=%+v err=%v", found, got, err)
	}
	if _, found, err := store.Block(BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 1}); err != nil || found {
		t.Fatalf("MC from rejected batch persisted: found=%v err=%v", found, err)
	}
}

func TestRollbackInvalidationPersists(t *testing.T) {
	path := t.TempDir() + "/rollback.db"
	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	commitLinearStoreTransition(t, store, 1, 100, 1, 99, 0)
	commitLinearStoreTransition(t, store, 2, 102, 2, 101, 1)
	before, err := store.Status()
	if err != nil || before.LastMismatchAt != nil {
		t.Fatalf("new store has a mismatch timestamp: %+v, %v", before, err)
	}
	markedAt := time.Now().UTC()
	if _, err := store.MarkRollback(); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	status, err := store.Status()
	if err != nil {
		t.Fatal(err)
	}
	if status.CoverageValid || !status.RollbackDetected || status.LastMismatchAt == nil ||
		status.LastMismatchAt.Before(markedAt.Add(-time.Second)) || status.LastMismatchAt.After(time.Now()) {
		t.Fatalf("rollback quality was not durable: %+v", status)
	}
}

func commitLinearStoreTransition(t *testing.T, store *Store, mcSeqno uint32, mcUtime int64, shardSeqno uint32, shardUtime int64, shardTx int) {
	t.Helper()
	cp := testCheckpoint(mcSeqno, map[shardKey]uint32{{wc: 0, sh: testRootShard}: shardSeqno})
	stats := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: mcSeqno}, GenUtime: mcUtime, IsMC: true},
		{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: shardSeqno}, GenUtime: shardUtime, TxCount: shardTx},
	}
	if _, err := store.CommitForward(stats, cp, mcUtime, true); err != nil {
		t.Fatal(err)
	}
}

// A batched backfill must leave exactly the state the same steps produce one at
// a time, and must reject a batch whose steps are not adjacent and descending.
func TestCommitBackfillBatchMatchesStepwiseCommits(t *testing.T) {
	shard := shardKey{wc: 0, sh: testRootShard}
	seed := func(store *Store) {
		anchor := []Stat{
			{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 10}, GenUtime: 200, IsMC: true},
			{ID: blockID(shard, 10), GenUtime: 200},
		}
		if _, err := store.CommitForward(anchor, testCheckpoint(10, map[shardKey]uint32{shard: 10}), 200, true); err != nil {
			t.Fatal(err)
		}
	}
	steps := make([]BackfillStep, 0, 5)
	for i := uint32(1); i <= 5; i++ {
		sq := 10 - i
		ut := int64(200 - i)
		steps = append(steps, BackfillStep{
			Stats: []Stat{
				{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: sq}, GenUtime: ut, IsMC: true},
				{ID: blockID(shard, sq), GenUtime: ut, TxCount: int(i)},
			},
			Checkpoint: testCheckpoint(sq, map[shardKey]uint32{shard: sq}),
			MCUtime:    ut,
			Lineage:    true,
		})
	}

	stepwise, err := OpenStore(t.TempDir() + "/stepwise.db")
	if err != nil {
		t.Fatal(err)
	}
	defer stepwise.Close()
	seed(stepwise)
	var want RecorderState
	for _, step := range steps {
		if want, err = stepwise.CommitBackfill(step.Stats, step.Checkpoint, step.MCUtime, step.Lineage); err != nil {
			t.Fatal(err)
		}
	}

	batched, err := OpenStore(t.TempDir() + "/batched.db")
	if err != nil {
		t.Fatal(err)
	}
	defer batched.Close()
	seed(batched)
	got, err := batched.CommitBackfillBatch(steps)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("batched state: got %+v want %+v", got, want)
	}

	gap, err := OpenStore(t.TempDir() + "/gap.db")
	if err != nil {
		t.Fatal(err)
	}
	defer gap.Close()
	seed(gap)
	if _, err := gap.CommitBackfillBatch([]BackfillStep{steps[0], steps[2]}); err == nil {
		t.Fatal("expected a non-contiguous batch to be rejected")
	}
	status, err := gap.Status()
	if err != nil {
		t.Fatal(err)
	}
	if status.CoverageStart != nil || status.CoverageEnd != nil {
		t.Fatalf("rejected batch must not be partially applied: %+v", status)
	}
}
