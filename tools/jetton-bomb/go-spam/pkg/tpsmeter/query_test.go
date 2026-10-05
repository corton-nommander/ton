package tpsmeter

import (
	"testing"
	"time"
)

func TestSummaryExact600SecondWindows(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/summary.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	start := time.Unix(1_700_000_000, 0).UTC()
	seedCoveredRun(t, store, start, 900, func(second int, _ uint32, _ int64) (int, []Stat) {
		tx := 1
		if second >= 123 && second < 723 {
			tx = 10
		}
		return 0, []Stat{{
			ID:      BlockID{Workchain: 0, Shard: testRootShard + 1, Seqno: uint32(second + 1)},
			TxCount: tx,
		}}
	})

	short, err := store.Summary(start, start.Add(599*time.Second), 600)
	if err != nil {
		t.Fatal(err)
	}
	if short.LastWindow != nil || short.MaxWindow != nil || short.MaxWindowStart != nil || short.MaxWindowEnd != nil {
		t.Fatalf("short run windows must be null: %+v", short)
	}

	exact, err := store.Summary(start, start.Add(600*time.Second), 600)
	if err != nil {
		t.Fatal(err)
	}
	exactTPS := float64(123+477*10) / 600
	if exact.LastWindow == nil || exact.MaxWindow == nil || *exact.LastWindow != exactTPS || *exact.MaxWindow != exactTPS {
		t.Fatalf("exact run: %+v, want TPS %.6f", exact, exactTPS)
	}
	if exact.MaxWindowStart == nil || !exact.MaxWindowStart.Equal(start) || exact.MaxWindowEnd == nil || !exact.MaxWindowEnd.Equal(start.Add(600*time.Second)) {
		t.Fatalf("exact max bounds: %+v", exact)
	}

	full, err := store.Summary(start, start.Add(900*time.Second), 600)
	if err != nil {
		t.Fatal(err)
	}
	if full.RunAverage != 7 {
		t.Fatalf("run_avg = %.6f, want 7", full.RunAverage)
	}
	if full.LastWindow == nil || *full.LastWindow != float64(423*10+177)/600 {
		t.Fatalf("last_window = %v", full.LastWindow)
	}
	if full.MaxWindow == nil || *full.MaxWindow != 10 {
		t.Fatalf("max_window = %v, want 10", full.MaxWindow)
	}
	wantMaxStart := start.Add(123 * time.Second)
	if full.MaxWindowStart == nil || !full.MaxWindowStart.Equal(wantMaxStart) || full.MaxWindowEnd == nil || !full.MaxWindowEnd.Equal(wantMaxStart.Add(600*time.Second)) {
		t.Fatalf("max bounds: %+v", full)
	}
	if !full.Complete {
		t.Fatal("fully covered summary marked incomplete")
	}
}

func TestBlockRatesSplitMasterchainFromShards(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/blockrate.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	start := time.Unix(1_700_000_000, 0).UTC()
	// seedCoveredRun always emits one masterchain and one root-shard block per
	// second; a second shard block lands on even seconds only.
	seedCoveredRun(t, store, start, 120, func(second int, _ uint32, _ int64) (int, []Stat) {
		if second%2 != 0 {
			return 1, nil
		}
		return 1, []Stat{{ID: BlockID{Workchain: 0, Shard: testRootShard + 1, Seqno: uint32(second + 2)}}}
	})

	summary, err := store.Summary(start, start.Add(120*time.Second), 60)
	if err != nil {
		t.Fatal(err)
	}
	if summary.RunBlockRate.MC != 1 || summary.RunBlockRate.WC != 180.0/120.0 || summary.RunBlockRate.Total != 300.0/120.0 {
		t.Fatalf("run block rate: %+v", summary.RunBlockRate)
	}
	if summary.LastWindowBlockRate == nil || summary.LastWindowBlockRate.MC != 1 || summary.LastWindowBlockRate.WC != 90.0/60.0 {
		t.Fatalf("last window block rate: %+v", summary.LastWindowBlockRate)
	}

	series, err := store.Series(start, start.Add(120*time.Second), 60, 60)
	if err != nil {
		t.Fatal(err)
	}
	if len(series.Points) != 2 {
		t.Fatalf("points = %d, want 2", len(series.Points))
	}
	for _, point := range series.Points {
		if point.MCBlockRate != 1 || point.WCBlockRate != 90.0/60.0 || point.BlockRate != 150.0/60.0 {
			t.Fatalf("point %v: %+v", point.Timestamp, point)
		}
	}
}

// A split doubles the summed WC rate while every shard keeps its cadence, so
// WC follows the split and WCPerShard stays flat.
func TestBlockRatesPerShardAcrossSplit(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/pershard.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	start := time.Unix(1_700_000_000, 0).UTC()
	const seconds = 120
	const splitAt = 60
	root := shardKey{wc: 0, sh: testRootShard}
	sibling := shardKey{wc: 0, sh: testRootShard + 1}
	shards := map[shardKey]uint32{root: 1}
	anchor := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 1}, GenUtime: start.Unix() - 1, IsMC: true},
		{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: 1}, GenUtime: start.Unix() - 1},
	}
	if _, err := store.CommitForward(anchor, testCheckpoint(1, shards), start.Unix()-1, true); err != nil {
		t.Fatal(err)
	}
	// One block per shard per second; the second shard appears at splitAt. The
	// closing commit at second == seconds covers the whole queried range.
	for second := 0; second <= seconds; second++ {
		utime := start.Unix() + int64(second)
		mcSeqno := uint32(second + 2)
		stats := []Stat{
			{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: mcSeqno}, GenUtime: utime, IsMC: true},
			{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: uint32(second + 2)}, GenUtime: utime},
		}
		shards[root] = uint32(second + 2)
		if second >= splitAt {
			stats = append(stats, Stat{
				ID: BlockID{Workchain: 0, Shard: testRootShard + 1, Seqno: uint32(second + 2)}, GenUtime: utime,
			})
			shards[sibling] = uint32(second + 2)
		}
		if _, err := store.CommitForward(stats, testCheckpoint(mcSeqno, shards), utime, true); err != nil {
			t.Fatal(err)
		}
	}

	summary, err := store.Summary(start, start.Add(seconds*time.Second), 60)
	if err != nil {
		t.Fatal(err)
	}
	run := summary.RunBlockRate
	if run.MC != 1 || run.WC != 1.5 {
		t.Fatalf("run block rate: %+v", run)
	}
	if run.WCPerShard == nil || *run.WCPerShard != 1 {
		t.Fatalf("run wc_per_shard = %v, want 1", derefRate(run.WCPerShard))
	}
	if run.Shards == nil || *run.Shards != 1.5 {
		t.Fatalf("run shards = %v, want 1.5", derefRate(run.Shards))
	}
	last := summary.LastWindowBlockRate
	if last == nil || last.WC != 2 {
		t.Fatalf("last window block rate: %+v", last)
	}
	if last.WCPerShard == nil || *last.WCPerShard != 1 {
		t.Fatalf("last window wc_per_shard = %v, want 1", derefRate(last.WCPerShard))
	}
	if last.Shards == nil || *last.Shards != 2 {
		t.Fatalf("last window shards = %v, want 2", derefRate(last.Shards))
	}
}

func derefRate(value *float64) any {
	if value == nil {
		return "nil"
	}
	return *value
}

func TestQueryRangeIsBounded(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/bounded.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	start := time.Unix(1_700_000_000, 0).UTC()
	bound := time.Duration(maxQueryRangeSeconds) * time.Second
	if _, err := store.Series(start, start.Add(bound), 60, 0); err != nil {
		t.Fatalf("range at the bound: %v", err)
	}
	if _, err := store.Series(start, start.Add(bound+time.Second), 60, 0); err == nil {
		t.Fatal("range longer than the bound was accepted")
	}
}

func TestSeriesTotalIsMCPlusAllWorkchains(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/series.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	start := time.Unix(1_700_010_000, 0).UTC()
	seedCoveredRun(t, store, start, 10, func(second int, _ uint32, _ int64) (int, []Stat) {
		seqno := uint32(second + 1)
		return 2, []Stat{
			{ID: BlockID{Workchain: 0, Shard: testRootShard + 1, Seqno: seqno}, TxCount: 1},
			{ID: BlockID{Workchain: 1, Shard: 2, Seqno: seqno}, TxCount: 2},
		}
	})
	series, err := store.Series(start, start.Add(10*time.Second), 5, 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(series.Points) != 6 {
		t.Fatalf("points = %d, want 6", len(series.Points))
	}
	for _, point := range series.Points {
		if point.MC != 2 || point.WC != 3 || point.Total != 5 {
			t.Fatalf("point %+v", point)
		}
	}
	if !series.Complete {
		t.Fatal("fully covered series marked incomplete")
	}
}

func TestGapAndPartialRemainIncompleteAfterReopen(t *testing.T) {
	path := t.TempDir() + "/quality.db"
	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	start := time.Unix(1_700_020_000, 0).UTC()
	anchorCP := testCheckpoint(1, map[shardKey]uint32{{wc: 0, sh: testRootShard}: 1})
	anchor := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 1}, GenUtime: start.Unix() - 1, IsMC: true},
		{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: 1}, GenUtime: start.Unix() - 1},
	}
	if _, err := store.CommitForward(anchor, anchorCP, start.Unix()-1, true); err != nil {
		t.Fatal(err)
	}
	for seqno := uint32(2); seqno <= 5; seqno++ {
		utime := start.Unix() + int64(seqno-1)
		cp := testCheckpoint(seqno, map[shardKey]uint32{{wc: 0, sh: testRootShard}: seqno})
		stats := []Stat{
			{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: seqno}, GenUtime: utime, TxCount: 1, IsMC: true},
			{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: seqno}, GenUtime: utime},
		}
		lineage := seqno != 3
		if _, err := store.CommitForward(stats, cp, utime, lineage); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := store.RecordBlock(Stat{
		ID: BlockID{Workchain: 0, Shard: testRootShard + 1, Seqno: 99}, GenUtime: start.Unix() + 3, TxCount: 1, Partial: true,
	}); err != nil {
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
	gapOnly, err := store.Summary(start, start.Add(3*time.Second), 1)
	if err != nil {
		t.Fatal(err)
	}
	if gapOnly.Complete || gapOnly.PartialBlocks != 0 {
		t.Fatalf("durable gap was hidden after reopen: %+v", gapOnly)
	}
	summary, err := store.Summary(start, start.Add(4*time.Second), 1)
	if err != nil {
		t.Fatal(err)
	}
	if summary.Complete || summary.PartialBlocks != 1 {
		t.Fatalf("gap/partial quality after reopen: %+v", summary)
	}
	series, err := store.Series(start, start.Add(4*time.Second), 1, 1)
	if err != nil {
		t.Fatal(err)
	}
	if series.Complete || series.PartialBlocks != 1 {
		t.Fatalf("series gap/partial quality after reopen: %+v", series)
	}
}

func TestResetPreservesHistoryAndLatestEpochWinsReusedData(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/epochs.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	start := time.Unix(1_700_030_000, 0).UTC()
	seedCoveredRun(t, store, start, 3, func(_ int, _ uint32, _ int64) (int, []Stat) {
		return 2, nil
	})
	reusedID := BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 2}
	oldBlock, found, err := store.Block(reusedID)
	if err != nil || !found || oldBlock.TxCount != 2 {
		t.Fatalf("epoch 1 block: found=%v block=%+v err=%v", found, oldBlock, err)
	}

	if err := store.ResetLiveChain(); err != nil {
		t.Fatal(err)
	}
	status, err := store.Status()
	if err != nil {
		t.Fatal(err)
	}
	if status.Epoch != 2 || status.Blocks != 0 || status.CoverageStart != nil || status.CoverageEnd != nil {
		t.Fatalf("fresh active epoch status: %+v", status)
	}
	state, err := store.LoadState()
	if err != nil {
		t.Fatal(err)
	}
	if state.Checkpoint.HaveMC || state.Coverage.HaveAnchor {
		t.Fatalf("reset retained live state: %+v", state)
	}
	historical, err := store.Series(start, start.Add(3*time.Second), 1, 1)
	if err != nil {
		t.Fatal(err)
	}
	if !historical.Complete || historical.IncludesActiveEpoch || historical.EpochOverlap || len(historical.Points) != 3 {
		t.Fatalf("historical series after reset: %+v", historical)
	}
	for _, point := range historical.Points {
		if point.Total != 2 {
			t.Fatalf("historical point after reset: %+v", point)
		}
	}
	if got, found, err := store.Block(reusedID); err != nil || !found || got != oldBlock {
		t.Fatalf("historical block after reset: found=%v block=%+v err=%v", found, got, err)
	}

	if err := store.ResetLiveChain(); err != nil {
		t.Fatal(err)
	}
	status, err = store.Status()
	if err != nil {
		t.Fatal(err)
	}
	if status.Epoch != 2 {
		t.Fatalf("idempotent empty reset advanced to epoch %d", status.Epoch)
	}

	seedCoveredRun(t, store, start, 3, func(_ int, _ uint32, _ int64) (int, []Stat) {
		return 7, nil
	})
	latest, found, err := store.Block(reusedID)
	if err != nil || !found || latest.TxCount != 7 {
		t.Fatalf("latest reused block: found=%v block=%+v err=%v", found, latest, err)
	}
	epochOne, found, err := store.BlockAtEpoch(1, reusedID)
	if err != nil || !found || epochOne != oldBlock {
		t.Fatalf("epoch 1 reused block: found=%v block=%+v err=%v", found, epochOne, err)
	}
	epochTwo, found, err := store.BlockAtEpoch(2, reusedID)
	if err != nil || !found || epochTwo != latest {
		t.Fatalf("epoch 2 reused block: found=%v block=%+v err=%v", found, epochTwo, err)
	}
	overlap, err := store.Summary(start, start.Add(3*time.Second), 1)
	if err != nil {
		t.Fatal(err)
	}
	if overlap.RunAverage != 7 || overlap.Complete || !overlap.IncludesActiveEpoch || !overlap.EpochOverlap || len(overlap.Epochs) != 2 || overlap.Epochs[0] != 1 || overlap.Epochs[1] != 2 {
		t.Fatalf("latest-epoch-wins summary: %+v", overlap)
	}
}

func TestMeterHydrationUsesOnlyActiveEpoch(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/hydrate-epochs.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	start := time.Unix(1_700_040_000, 0).UTC()
	seedCoveredRun(t, store, start, 5, func(_ int, _ uint32, _ int64) (int, []Stat) {
		return 100, nil
	})
	if err := store.ResetLiveChain(); err != nil {
		t.Fatal(err)
	}
	seedCoveredRun(t, store, start, 5, func(_ int, _ uint32, _ int64) (int, []Stat) {
		return 3, nil
	})

	meter := New(nopSource{}, Options{Recorder: store, Windows: []int{5}})
	snapshot := meter.Snapshot()
	if !snapshot.WindowsReady || len(snapshot.Windows) != 1 || snapshot.Windows[0].TotalTx != 15 || snapshot.TotalTx != 15 {
		t.Fatalf("active-only hydration: %+v", snapshot)
	}
	status, err := store.Status()
	if err != nil {
		t.Fatal(err)
	}
	if status.Epoch != 2 || snapshot.StoredBlocks != status.Blocks {
		t.Fatalf("active-only status: status=%+v snapshot=%+v", status, snapshot)
	}
}

func TestSeriesIncludesNonOverlappingEpochs(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/non-overlap.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err := store.RecordBlock(Stat{ID: BlockID{Workchain: 0, Shard: 1, Seqno: 1}, GenUtime: 100, TxCount: 2}); err != nil {
		t.Fatal(err)
	}
	if err := store.ResetLiveChain(); err != nil {
		t.Fatal(err)
	}
	if _, err := store.RecordBlock(Stat{ID: BlockID{Workchain: 0, Shard: 1, Seqno: 1}, GenUtime: 110, TxCount: 5}); err != nil {
		t.Fatal(err)
	}

	series, err := store.Series(time.Unix(99, 0), time.Unix(112, 0), 1, 1)
	if err != nil {
		t.Fatal(err)
	}
	if series.EpochOverlap || len(series.Epochs) != 2 || series.Epochs[0] != 1 || series.Epochs[1] != 2 {
		t.Fatalf("non-overlapping epoch provenance: %+v", series)
	}
	points := make(map[int64]float64, len(series.Points))
	for _, point := range series.Points {
		points[point.Timestamp.Unix()] = point.Total
	}
	if points[101] != 2 || points[111] != 5 {
		t.Fatalf("non-overlapping epoch points: at 101=%v at 111=%v", points[101], points[111])
	}
}

func seedCoveredRun(t *testing.T, store *Store, start time.Time, seconds int, blocks func(int, uint32, int64) (int, []Stat)) {
	t.Helper()
	shards := map[shardKey]uint32{{wc: 0, sh: testRootShard}: 1}
	anchorCP := testCheckpoint(1, shards)
	anchor := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 1}, GenUtime: start.Unix() - 1, IsMC: true},
		{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: 1}, GenUtime: start.Unix() - 1},
	}
	if _, err := store.CommitForward(anchor, anchorCP, start.Unix()-1, true); err != nil {
		t.Fatal(err)
	}
	for second := 0; second < seconds; second++ {
		mcSeqno := uint32(second + 2)
		utime := start.Unix() + int64(second)
		mcTx, extra := blocks(second, mcSeqno, utime)
		stats := []Stat{{
			ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: mcSeqno}, GenUtime: utime, TxCount: mcTx, IsMC: true,
		}, {
			ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: uint32(second + 2)}, GenUtime: utime,
		}}
		for _, stat := range extra {
			stat.GenUtime = utime
			stats = append(stats, stat)
		}
		shards[shardKey{wc: 0, sh: testRootShard}] = uint32(second + 2)
		if _, err := store.CommitForward(stats, testCheckpoint(mcSeqno, shards), utime, true); err != nil {
			t.Fatal(err)
		}
	}
	finalSeqno := uint32(seconds + 2)
	finalUtime := start.Unix() + int64(seconds)
	shards[shardKey{wc: 0, sh: testRootShard}] = uint32(seconds + 2)
	final := []Stat{
		{ID: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: finalSeqno}, GenUtime: finalUtime, IsMC: true},
		{ID: BlockID{Workchain: 0, Shard: testRootShard, Seqno: uint32(seconds + 2)}, GenUtime: finalUtime},
	}
	if _, err := store.CommitForward(final, testCheckpoint(finalSeqno, shards), finalUtime, true); err != nil {
		t.Fatal(err)
	}
}
