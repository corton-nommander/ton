package main

import (
	"encoding/json"
	"io"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/tpsmeter"
)

func TestValidateListenAddressRequiresLoopback(t *testing.T) {
	valid := []string{"127.0.0.1:8091", "127.0.0.2:1", "localhost:65535", "[::1]:8091"}
	for _, address := range valid {
		if err := validateListenAddress(address); err != nil {
			t.Errorf("validateListenAddress(%q): %v", address, err)
		}
	}
	invalid := []string{"127.0.0.1", ":8091", "0.0.0.0:8091", "192.0.2.1:8091", "example.com:8091", "127.0.0.1:0", "127.0.0.1:65536"}
	for _, address := range invalid {
		if err := validateListenAddress(address); err == nil {
			t.Errorf("validateListenAddress(%q) unexpectedly succeeded", address)
		}
	}
}

func TestResetChainMaintenanceMode(t *testing.T) {
	path := t.TempDir() + "/reset.db"
	id := tpsmeter.BlockID{Workchain: 0, Shard: 1, Seqno: 1}
	stat := tpsmeter.Stat{ID: id, GenUtime: 100, TxCount: 3}
	store, err := tpsmeter.OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.RecordBlock(stat); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	// Reset mode must not try to open this inherited source configuration.
	t.Setenv("LS_CONFIG", "/does/not/exist.json")
	if err := runArgs([]string{"--reset-chain", "--store", path}); err != nil {
		t.Fatal(err)
	}
	store, err = tpsmeter.OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	status, err := store.Status()
	if err != nil {
		t.Fatal(err)
	}
	if status.Epoch != 2 || status.Blocks != 0 {
		t.Fatalf("reset active status: %+v", status)
	}
	got, found, err := store.BlockAtEpoch(1, id)
	if err != nil || !found || got != stat {
		t.Fatalf("reset historical block: found=%v block=%+v err=%v", found, got, err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	for _, args := range [][]string{
		{"--reset-chain"},
		{"--reset-chain", "--store", path, "--listen", "127.0.0.1:8091"},
		{"--reset-chain", "--store", path, "--api", "https://example.invalid"},
		{"--reset-chain", "--store", path, "--ls-config", "/tmp/config.json"},
	} {
		if err := runArgs(args); err == nil || !strings.Contains(err.Error(), "--reset-chain") {
			t.Errorf("runArgs(%v) error = %v", args, err)
		}
	}
}

func TestImportStoreMaintenanceMode(t *testing.T) {
	destinationPath := t.TempDir() + "/destination.db"
	sourcePath := t.TempDir() + "/source.db"
	source, err := tpsmeter.OpenStore(sourcePath)
	if err != nil {
		t.Fatal(err)
	}
	id := tpsmeter.BlockID{Workchain: 0, Shard: 1, Seqno: 1}
	stat := tpsmeter.Stat{ID: id, GenUtime: 100, TxCount: 3}
	if _, err := source.RecordBlock(stat); err != nil {
		t.Fatal(err)
	}
	if err := source.Close(); err != nil {
		t.Fatal(err)
	}

	t.Setenv("LS_CONFIG", "/does/not/exist.json")
	if err := runArgs([]string{"--import-store", sourcePath, "--store", destinationPath}); err != nil {
		t.Fatal(err)
	}
	destination, err := tpsmeter.OpenStore(destinationPath)
	if err != nil {
		t.Fatal(err)
	}
	defer destination.Close()
	if got, found, err := destination.BlockAtEpoch(2, id); err != nil || !found || got != stat {
		t.Fatalf("imported block: found=%v got=%+v err=%v", found, got, err)
	}

	for _, args := range [][]string{
		{"--import-store", sourcePath},
		{"--import-store", sourcePath, "--store", destinationPath, "--listen", "127.0.0.1:8091"},
		{"--import-store", sourcePath, "--store", destinationPath, "--reset-chain"},
	} {
		if err := runArgs(args); err == nil || !strings.Contains(err.Error(), "--") {
			t.Errorf("runArgs(%v) error = %v", args, err)
		}
	}
}

func TestSummaryMaintenanceMode(t *testing.T) {
	path := t.TempDir() + "/summary.db"
	store, err := tpsmeter.OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	const rootShard = int64(-1 << 63)
	start := time.Unix(1_700_050_000, 0).UTC()
	identity := tpsmeter.BlockIdentity{RootHash: [32]byte{1}, FileHash: [32]byte{2}}
	checkpoint := tpsmeter.Checkpoint{
		HaveMC:     true,
		MCSeqno:    1,
		MCIdentity: identity,
		Shards:     map[tpsmeter.ShardID]uint32{{Workchain: 0, Shard: rootShard}: 1},
	}
	if _, err := store.CommitForward([]tpsmeter.Stat{{
		ID:       tpsmeter.BlockID{Workchain: -1, Shard: rootShard, Seqno: 1},
		GenUtime: start.Unix() - 1,
		IsMC:     true,
	}, {
		ID:       tpsmeter.BlockID{Workchain: 0, Shard: rootShard, Seqno: 1},
		GenUtime: start.Unix() - 1,
	}}, checkpoint, start.Unix()-1, true); err != nil {
		t.Fatal(err)
	}
	for second := 0; second < 600; second++ {
		seqno := uint32(second + 2)
		utime := start.Unix() + int64(second)
		checkpoint.MCSeqno = seqno
		checkpoint.Shards[tpsmeter.ShardID{Workchain: 0, Shard: rootShard}] = seqno
		if _, err := store.CommitForward([]tpsmeter.Stat{{
			ID:       tpsmeter.BlockID{Workchain: -1, Shard: rootShard, Seqno: seqno},
			GenUtime: utime,
			IsMC:     true,
		}, {
			ID:       tpsmeter.BlockID{Workchain: 0, Shard: rootShard, Seqno: seqno},
			GenUtime: utime,
			TxCount:  10,
		}}, checkpoint, utime, true); err != nil {
			t.Fatal(err)
		}
	}
	checkpoint.MCSeqno = 602
	checkpoint.Shards[tpsmeter.ShardID{Workchain: 0, Shard: rootShard}] = 602
	if _, err := store.CommitForward([]tpsmeter.Stat{{
		ID:       tpsmeter.BlockID{Workchain: -1, Shard: rootShard, Seqno: 602},
		GenUtime: start.Unix() + 600,
		IsMC:     true,
	}, {
		ID:       tpsmeter.BlockID{Workchain: 0, Shard: rootShard, Seqno: 602},
		GenUtime: start.Unix() + 600,
	}}, checkpoint, start.Unix()+600, true); err != nil {
		t.Fatal(err)
	}
	if err := store.ResetLiveChain(); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	outputReader, outputWriter, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	originalStdout := os.Stdout
	os.Stdout = outputWriter
	t.Setenv("LS_CONFIG", "/does/not/exist.json")
	runErr := runArgs([]string{
		"--summary",
		"--store", path,
		"--start", start.Format(time.RFC3339),
		"--end", start.Add(600 * time.Second).Format(time.RFC3339),
		"--windows", "60,600",
	})
	os.Stdout = originalStdout
	if err := outputWriter.Close(); err != nil {
		t.Fatal(err)
	}
	output, readErr := io.ReadAll(outputReader)
	if err := outputReader.Close(); err != nil {
		t.Fatal(err)
	}
	if runErr != nil {
		t.Fatal(runErr)
	}
	if readErr != nil {
		t.Fatal(readErr)
	}

	var summaries []tpsmeter.SummaryResult
	if err := json.Unmarshal(output, &summaries); err != nil {
		t.Fatalf("decode output %q: %v", output, err)
	}
	if len(summaries) != 2 || summaries[0].WindowSize != 60 || summaries[1].WindowSize != 600 {
		t.Fatalf("summaries: %+v", summaries)
	}
	for _, summary := range summaries {
		if summary.RunAverage != 10 || summary.MaxWindow == nil || *summary.MaxWindow != 10 || !summary.Complete {
			t.Fatalf("summary: %+v", summary)
		}
		if summary.IncludesActiveEpoch || len(summary.Epochs) != 1 || summary.Epochs[0] != 1 {
			t.Fatalf("historical epoch provenance: %+v", summary)
		}
	}

	for _, args := range [][]string{
		{"--summary"},
		{"--summary", "--store", path, "--start", start.Format(time.RFC3339), "--end", start.Add(time.Minute).Format(time.RFC3339), "--api", "https://example.invalid"},
		{"--start", start.Format(time.RFC3339)},
	} {
		if err := runArgs(args); err == nil || !strings.Contains(err.Error(), "--") {
			t.Errorf("runArgs(%v) error = %v", args, err)
		}
	}
}
