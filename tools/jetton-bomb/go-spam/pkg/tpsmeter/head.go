package tpsmeter

import (
	"context"
	"fmt"
	"sort"
	"sync"
)

// HeadTip is a block header from one coherent masterchain view. Shards use the
// canonical 16-digit unsigned hexadecimal TON shard ID; gen_utime is quoted to
// match the block-tip JSON consumed by the cockpit.
type HeadTip struct {
	Workchain int32  `json:"workchain"`
	Shard     string `json:"shard"`
	Seqno     uint32 `json:"seqno"`
	GenUtime  int64  `json:"gen_utime,string"`
}

type LiveHead struct {
	MC     HeadTip   `json:"mc"`
	Shards []HeadTip `json:"shards"`
	Source string    `json:"source"`
}

// LiveHead reads the current MC and only the shard tops that MC references.
// Unlike the TPS checkpoint, this view can stay current while the meter catches
// up. Header-only calls avoid the expensive per-block transaction enumeration.
func (m *Meter) LiveHead(ctx context.Context) (LiveHead, error) {
	latest, err := m.src.LatestMasterchainSeqno(ctx)
	if err != nil {
		return LiveHead{}, fmt.Errorf("latest masterchain: %w", err)
	}
	if latest == 0 {
		return LiveHead{}, fmt.Errorf("latest masterchain has no seqno")
	}
	shards, err := m.src.Shards(ctx, latest)
	if err != nil {
		return LiveHead{}, fmt.Errorf("shards of MC %d: %w", latest, err)
	}
	var basechain []BlockID
	for _, shard := range shards {
		if shard.Workchain == 0 {
			basechain = append(basechain, shard)
		}
	}
	current, err := normalizeShardTops(basechain)
	if err != nil {
		return LiveHead{}, fmt.Errorf("shards of MC %d: %w", latest, err)
	}
	ids := []BlockID{{Workchain: mcWorkchain, Shard: mcShard, Seqno: latest}}
	for key, seqno := range current {
		ids = append(ids, blockID(key, seqno))
	}
	sort.Slice(ids[1:], func(i, j int) bool {
		return uint64(ids[i+1].Shard) < uint64(ids[j+1].Shard)
	})

	tips := make([]HeadTip, len(ids))
	errs := make([]error, len(ids))
	sem := make(chan struct{}, m.opts.Workers)
	var wg sync.WaitGroup
	for i, id := range ids {
		wg.Add(1)
		sem <- struct{}{}
		go func(i int, id BlockID) {
			defer wg.Done()
			defer func() { <-sem }()
			utime, err := m.src.BlockUtime(ctx, id)
			if err != nil {
				errs[i] = fmt.Errorf("block (%d,%016X,%d): %w", id.Workchain, uint64(id.Shard), id.Seqno, err)
				return
			}
			if utime <= 0 {
				errs[i] = fmt.Errorf("block (%d,%016X,%d) has no gen_utime", id.Workchain, uint64(id.Shard), id.Seqno)
				return
			}
			tips[i] = HeadTip{Workchain: id.Workchain, Shard: fmt.Sprintf("%016X", uint64(id.Shard)), Seqno: id.Seqno, GenUtime: utime}
		}(i, id)
	}
	wg.Wait()
	for _, err := range errs {
		if err != nil {
			return LiveHead{}, err
		}
	}
	source := "tpsmeter live block headers"
	if _, ok := m.src.(*LSSource); ok {
		source = "tpsmeter liteserver head"
	}
	return LiveHead{MC: tips[0], Shards: tips[1:], Source: source}, nil
}
