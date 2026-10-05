package main

import (
	"fmt"
	"testing"

	"github.com/victor/jetton-bomb-go-spam/pkg/nativerpc"
	"github.com/victor/jetton-bomb-go-spam/pkg/sidechain"
)

func topologySnapshot(depth int) *nativerpc.Snapshot {
	s := &nativerpc.Snapshot{}
	for lane := 0; lane < 1<<depth; lane++ {
		id := (uint64(lane)<<uint(64-depth) | uint64(1)<<uint(63-depth))
		s.Shards = append(s.Shards, sidechain.BlockIdentity{
			Workchain: 0, Shard: fmt.Sprintf("%016X", id), Seqno: uint32(123 + lane),
		})
	}
	return s
}

func TestRequireTopologyAcceptsEveryCompleteFixedLayout(t *testing.T) {
	for depth := 1; depth <= 3; depth++ {
		t.Run(fmt.Sprintf("depth%d", depth), func(t *testing.T) {
			snap := topologySnapshot(depth)
			// Block order and per-leaf sequence numbers can differ. Only the
			// requested full workchain-zero partition must remain fixed.
			for left, right := 0, len(snap.Shards)-1; left < right; left, right = left+1, right-1 {
				snap.Shards[left], snap.Shards[right] = snap.Shards[right], snap.Shards[left]
			}
			snap.Shards = append(snap.Shards,
				sidechain.BlockIdentity{Workchain: -1, Shard: "8000000000000000"},
				sidechain.BlockIdentity{Workchain: 1, Shard: "ignored-other-workchain"},
			)
			if err := requireTopology(snap, depth); err != nil {
				t.Fatalf("complete depth%d partition rejected: %v", depth, err)
			}
		})
	}
}

func TestRequireTopologyRejectsIncompleteGenesisSplitsAndChangedLayouts(t *testing.T) {
	tests := []struct {
		name   string
		mutate func(*nativerpc.Snapshot)
	}{
		{"missing leaf", func(s *nativerpc.Snapshot) { s.Shards = s.Shards[:3] }},
		{"duplicated leaf", func(s *nativerpc.Snapshot) { s.Shards[3] = s.Shards[2] }},
		{"extra duplicate leaf", func(s *nativerpc.Snapshot) { s.Shards = append(s.Shards, s.Shards[0]) }},
		{"mixed depths during split", func(s *nativerpc.Snapshot) { s.Shards[0].Shard = "4000000000000000" }},
		{"root shard", func(s *nativerpc.Snapshot) { s.Shards[0].Shard = "8000000000000000" }},
		{"overdeep child", func(s *nativerpc.Snapshot) { s.Shards[0].Shard = "1000000000000000" }},
		{"zero shard", func(s *nativerpc.Snapshot) { s.Shards[0].Shard = "0000000000000000" }},
		{"malformed shard", func(s *nativerpc.Snapshot) { s.Shards[0].Shard = "garbage" }},
		{"empty shard", func(s *nativerpc.Snapshot) { s.Shards[0].Shard = "" }},
		{"uint64 overflow", func(s *nativerpc.Snapshot) { s.Shards[0].Shard = "10000000000000000" }},
		{"wrong delimiter depth", func(s *nativerpc.Snapshot) { s.Shards[0].Shard = "2000000000000001" }},
		{"other workchain cannot replace leaf", func(s *nativerpc.Snapshot) { s.Shards[3].Workchain = 1 }},
		{"same numeric leaf with different case", func(s *nativerpc.Snapshot) { s.Shards[3].Shard = "a000000000000000" }},
		{"same numeric leaf with leading zero", func(s *nativerpc.Snapshot) { s.Shards[3].Shard = "0A000000000000000" }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			snap := topologySnapshot(2)
			test.mutate(snap)
			if err := requireTopology(snap, 2); err == nil {
				t.Fatal("incomplete, duplicated, or changed topology accepted")
			}
		})
	}
	if err := requireTopology(topologySnapshot(1), 2); err == nil {
		t.Fatal("two genesis leaves accepted as completed four-leaf layout")
	}
	if err := requireTopology(topologySnapshot(3), 2); err == nil {
		t.Fatal("different complete layout accepted")
	}
}

func TestRequireTopologyRejectsMissingSnapshotAndUnsupportedDepth(t *testing.T) {
	if err := requireTopology(nil, 2); err == nil {
		t.Fatal("nil snapshot accepted")
	}
	if err := requireTopology(&nativerpc.Snapshot{}, 2); err == nil {
		t.Fatal("empty shard topology accepted")
	}
	for _, depth := range []int{-1, 0, 4, 60} {
		if err := requireTopology(topologySnapshot(2), depth); err == nil {
			t.Fatalf("unsupported depth %d accepted", depth)
		}
	}
}
