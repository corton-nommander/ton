package tpsmeter

import (
	"context"
	"fmt"
	"sync"
	"time"
)

const (
	defaultPollInterval = 500 * time.Millisecond
	defaultWorkers      = 16
	defaultHistoryMax   = 65536
	defaultMaxCatchup   = 1024
	defaultMCMaxCatchup = 64
)

// Options configures a Meter. Catch-up limits bound work per poll; persistent
// traversal always resumes from the oldest missing block and never skips data.
type Options struct {
	PollInterval time.Duration
	Windows      []int
	Workers      int
	HistoryMax   int
	MaxCatchup   int // retained as tuning metadata; canonical shard transitions are never truncated
	MCMaxCatchup int
	Recorder     Recorder
	Backfill     bool
}

func (o Options) maxWindow() int {
	m := 0
	for _, w := range o.Windows {
		if w > m {
			m = w
		}
	}
	return m
}

// Stat is one recorded block (masterchain or shard).
type Stat struct {
	ID       BlockID
	GenUtime int64
	TxCount  int
	Partial  bool
	IsMC     bool
}

type WindowTPS struct {
	WindowSec int     `json:"window_size"`
	TPS       float64 `json:"tps"`
	TotalTx   int     `json:"total_tx"`
	Blocks    int     `json:"blocks"`
	MCBlocks  int     `json:"mc_blocks"`
	WCBlocks  int     `json:"wc_blocks"`
	// Block rates are blocks per second over the window. WCBlockRate sums every
	// shard, so per-shard cadence is WCBlockRate/shards.
	BlockRate   float64 `json:"block_rate"`
	MCBlockRate float64 `json:"mc_block_rate"`
	WCBlockRate float64 `json:"wc_block_rate"`
	Filling     bool    `json:"filling"`
}

type Snapshot struct {
	Shards           int
	Windows          []WindowTPS
	SpanSec          int
	TotalTx          int
	Blocks           int
	MCBlocks         int
	WCBlocks         int
	MaxUtime         int64
	Polls            int
	Errors           int
	SkippedMC        int
	SkippedShards    int
	PartialBlocks    uint64
	StoredBlocks     uint64
	CoverageValid    bool
	RollbackDetected bool
	PendingBatch     bool
	SourceVerified   bool
	ForwardSynced    bool
	WindowsReady     bool
	LastErr          string

	BackfillActive      bool
	BackfillComplete    bool
	BackfillOldestUtime int64
	BackfillTargetUtime int64
}

type shardKey struct {
	wc int32
	sh int64
}

type historyEntry struct {
	ID       BlockID
	GenUtime int64
	TxCount  int
	MCBlocks int
	WCBlocks int
}

type Meter struct {
	src      Source
	opts     Options
	recorder Recorder

	commitMu sync.Mutex
	mu       sync.Mutex

	history        []historyEntry
	haveMC         bool
	lastMC         uint32
	lastMCIdentity BlockIdentity
	lastShard      map[shardKey]uint32
	activeShards   map[shardKey]bool
	seen           map[BlockID]bool
	coverage       CoverageState

	polls            int
	errCount         int
	lastErr          string
	skippedMC        int
	skippedShards    int
	partialBlocks    uint64
	storedBlocks     uint64
	forwardPending   bool
	backfillPending  bool
	initialStateBad  bool
	rollbackDetected bool
	sourceVerified   bool
	forwardSynced    bool
	windowsReady     bool
	started          time.Time
	maxUtime         int64

	backfillActive   bool
	backfillComplete bool
	backfillOldest   int64
	backfillTarget   int64
}

func New(src Source, opts Options) *Meter {
	if opts.PollInterval <= 0 {
		opts.PollInterval = defaultPollInterval
	}
	if len(opts.Windows) == 0 {
		opts.Windows = []int{60, 300, 900}
	}
	if opts.Workers <= 0 {
		opts.Workers = defaultWorkers
	}
	if opts.HistoryMax <= 0 {
		opts.HistoryMax = defaultHistoryMax
	}
	if opts.MaxCatchup <= 0 {
		opts.MaxCatchup = defaultMaxCatchup
	}
	if opts.MCMaxCatchup <= 0 {
		opts.MCMaxCatchup = defaultMCMaxCatchup
	}
	m := &Meter{
		src:              src,
		opts:             opts,
		recorder:         opts.Recorder,
		lastShard:        map[shardKey]uint32{},
		activeShards:     map[shardKey]bool{},
		seen:             map[BlockID]bool{},
		coverage:         CoverageState{LowShards: map[ShardID]uint32{}},
		started:          time.Now(),
		backfillActive:   opts.Backfill,
		backfillComplete: !opts.Backfill,
	}
	if opts.Recorder != nil {
		state, err := opts.Recorder.LoadState()
		if err != nil {
			m.initialStateBad = true
			m.recordErr(err)
		} else {
			m.mu.Lock()
			m.applyRecorderStateLocked(state)
			m.mu.Unlock()
			if state.Status.CoverageStart != nil && state.Status.CoverageEnd != nil {
				start := state.Coverage.HighUtime - int64(opts.maxWindow()) - 1
				if coveredStart := state.Coverage.LowUtime + 1; start < coveredStart {
					start = coveredStart
				}
				points, loadErr := opts.Recorder.LoadRecentSeconds(start, state.Coverage.HighUtime)
				if loadErr != nil {
					m.initialStateBad = true
					m.recordErr(loadErr)
				} else {
					m.mu.Lock()
					m.hydrateRecentLocked(points)
					m.windowsReady = true
					m.mu.Unlock()
				}
			}
		}
	}
	return m
}

func (m *Meter) Source() Source { return m.src }

func (m *Meter) Run(ctx context.Context) error {
	var backfill sync.WaitGroup
	if m.opts.Backfill {
		backfill.Add(1)
		go func() {
			defer backfill.Done()
			m.backfill(ctx)
		}()
	}
	defer backfill.Wait()
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		t0 := time.Now()
		m.pollOnce(ctx)
		wait := m.opts.PollInterval - time.Since(t0)
		if wait < 0 {
			wait = 0
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(wait):
		}
	}
}

func (m *Meter) RunFor(ctx context.Context, d time.Duration) error {
	ctx2, cancel := context.WithTimeout(ctx, d)
	defer cancel()
	err := m.Run(ctx2)
	if err == context.DeadlineExceeded || err == context.Canceled {
		return nil
	}
	return err
}

func (m *Meter) pollOnce(ctx context.Context) {
	latest, err := m.src.LatestMasterchainSeqno(ctx)
	if err != nil {
		m.failForward(err)
		m.countPoll()
		return
	}

	m.mu.Lock()
	first, last := !m.haveMC, m.lastMC
	storedIdentity := m.lastMCIdentity
	stateBad := m.initialStateBad
	rollback := m.rollbackDetected
	m.mu.Unlock()
	if stateBad {
		m.countPoll()
		return
	}
	if !first && latest < last {
		if !rollback {
			m.markChainInvalid(fmt.Errorf("chain top %d is below stored checkpoint %d", latest, last))
		}
		m.countPoll()
		return
	}
	if !first {
		liveIdentity, err := m.src.MasterchainIdentity(ctx, last)
		if err != nil {
			m.failForward(fmt.Errorf("verify stored MC %d identity: %w", last, err))
			m.countPoll()
			return
		}
		if liveIdentity != storedIdentity {
			m.markChainInvalid(fmt.Errorf("stored MC %d identity does not match live canonical block", last))
			m.countPoll()
			return
		}
	}
	m.mu.Lock()
	m.sourceVerified = true
	m.forwardSynced = !first && latest == last
	m.mu.Unlock()
	if !first && latest == last {
		m.mu.Lock()
		m.forwardPending = false
		m.forwardSynced = true
		m.mu.Unlock()
		m.countPoll()
		return
	}

	start, end := latest, latest
	if !first {
		start = last + 1
		end = latest
		if limit := uint64(m.opts.MCMaxCatchup); limit > 0 && uint64(end)-uint64(start)+1 > limit {
			end = start + uint32(limit) - 1
		}
	}

	// Walk [start, end] in three phases instead of one serial round-trip chain per masterchain block.
	// Step sq's predecessor tops are step sq-1's OWN tops, which come from Shards(sq-1) — a call that does
	// not depend on any block being read. So the whole batch's transition graph, and therefore every block
	// it needs, is computable before a single BlockInfo is fetched, and both network phases collapse into
	// one wide parallel wave each. Only the commits stay strictly sequential, which is what coverage and
	// the store's contiguity check require.
	steps, planErr := m.planForward(ctx, start, end)
	if len(steps) == 0 {
		if planErr != nil {
			m.failForward(planErr)
		}
		m.mu.Lock()
		m.forwardSynced = m.haveMC && m.lastMC == latest && !m.forwardPending
		m.mu.Unlock()
		m.countPoll()
		return
	}

	batch := m.planForwardTargets(steps)
	wave := make([]fetchTarget, 0, len(batch)*4)
	queued := make(map[BlockID]bool, len(batch)*4)
	for _, step := range batch {
		for _, target := range step.targets {
			if !queued[target.id] {
				queued[target.id] = true
				wave = append(wave, target)
			}
		}
	}
	stats, fetchErrs := m.fetchStatsResults(ctx, wave)
	fetched := make(map[BlockID]Stat, len(wave))
	fetchFailed := make(map[BlockID]error, len(wave))
	for i, target := range wave {
		if fetchErrs[i] != nil {
			fetchFailed[target.id] = fetchErrs[i]
			continue
		}
		fetched[target.id] = stats[i]
	}

	commitErr := m.commitForwardBatch(ctx, batch, fetched, fetchFailed)
	if commitErr == nil {
		commitErr = planErr
	}
	if commitErr != nil {
		m.failForward(commitErr)
	}
	m.mu.Lock()
	m.forwardSynced = m.haveMC && m.lastMC == latest && !m.forwardPending
	m.mu.Unlock()
	m.countPoll()
}

// forwardStep is one masterchain step's plan: the shard tops it publishes and its canonical identity.
// Both come from calls keyed only on the seqno, so a whole range of them resolves in parallel.
type forwardStep struct {
	seqno    uint32
	shards   map[shardKey]uint32
	identity BlockIdentity
}

// forwardBatchStep is a planned step with the blocks it contributes, plus the predecessor tops those
// blocks were derived from — kept so the commit can refuse to apply a transition whose predecessor is no
// longer the committed one.
type forwardBatchStep struct {
	step     forwardStep
	targets  []fetchTarget
	previous map[shardKey]uint32
	anchor   bool
	lineage  bool
}

// planForward resolves each masterchain step's shard tops and identity in parallel. A step that fails
// truncates the plan there and returns the prefix with the error: the walk may never skip a masterchain
// block, so the next poll resumes from the first one that did not resolve.
func (m *Meter) planForward(ctx context.Context, start, end uint32) ([]forwardStep, error) {
	count := int(end-start) + 1
	steps := make([]forwardStep, count)
	errs := make([]error, count)
	sem := make(chan struct{}, m.opts.Workers)
	var wg sync.WaitGroup
	for i := 0; i < count; i++ {
		wg.Add(1)
		sem <- struct{}{}
		go func(i int) {
			defer wg.Done()
			defer func() { <-sem }()
			sq := start + uint32(i)
			shards, err := m.src.Shards(ctx, sq)
			if err != nil {
				errs[i] = err
				return
			}
			current, err := normalizeShardTops(shards)
			if err != nil {
				errs[i] = err
				return
			}
			identity, err := m.src.MasterchainIdentity(ctx, sq)
			if err != nil {
				errs[i] = fmt.Errorf("resolve MC %d identity: %w", sq, err)
				return
			}
			steps[i] = forwardStep{seqno: sq, shards: current, identity: identity}
		}(i)
	}
	wg.Wait()
	for i, err := range errs {
		if err != nil {
			return steps[:i], err
		}
	}
	return steps, nil
}

// planForwardTargets turns resolved steps into per-step fetch lists, chaining each step's predecessor
// from the previous step's tops exactly as the serial walk did by re-reading the committed checkpoint.
func (m *Meter) planForwardTargets(steps []forwardStep) []forwardBatchStep {
	m.mu.Lock()
	previous := cloneShardMap(m.lastShard)
	anchor := !m.haveMC
	m.mu.Unlock()

	out := make([]forwardBatchStep, 0, len(steps))
	for _, step := range steps {
		targets := []fetchTarget{{id: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: step.seqno}, isMC: true}}
		lineage := true
		if anchor {
			for key, seqno := range step.shards {
				if seqno == 0 {
					continue
				}
				targets = append(targets, fetchTarget{id: blockID(key, seqno)})
			}
		} else {
			var shardTargets []fetchTarget
			shardTargets, lineage = transitionTargets(previous, step.shards)
			targets = append(targets, shardTargets...)
		}
		out = append(out, forwardBatchStep{
			step: step, targets: targets, previous: previous, anchor: anchor, lineage: lineage,
		})
		previous = step.shards
		anchor = false
	}
	return out
}

// commitForwardBatch applies planned steps in order, stopping at the first one that is not fully
// readable. Stopping (rather than skipping) keeps the checkpoint contiguous, so the next poll re-plans
// from the failed masterchain block and no data is ever passed over.
func (m *Meter) commitForwardBatch(
	ctx context.Context,
	batch []forwardBatchStep,
	fetched map[BlockID]Stat,
	fetchFailed map[BlockID]error,
) error {
	for _, planned := range batch {
		if err := ctx.Err(); err != nil {
			return err
		}
		stats := make([]Stat, 0, len(planned.targets))
		var missing error
		for _, target := range planned.targets {
			stat, ok := fetched[target.id]
			if !ok {
				if missing = fetchFailed[target.id]; missing == nil {
					missing = fmt.Errorf("block %+v was not fetched", target.id)
				}
				break
			}
			stats = append(stats, stat)
		}
		if missing != nil {
			return missing
		}
		mcUtime, ok := masterchainUtime(stats, planned.step.seqno)
		if !ok {
			return fmt.Errorf("fetched batch is missing MC %d", planned.step.seqno)
		}
		// The plan was built up front, so re-check that the state it assumed is still the committed one.
		// Without this a concurrent state change would let a transition be applied against a predecessor
		// that no longer holds — the one hazard batching introduces over the serial walk.
		if !m.planStillCurrent(planned) {
			return fmt.Errorf("committed state moved away from the planned predecessor of MC %d", planned.step.seqno)
		}
		cp := checkpointFor(planned.step.seqno, planned.step.shards, planned.step.identity)
		if err := m.commitForward(stats, cp, mcUtime, planned.lineage); err != nil {
			return err
		}
		if !planned.lineage {
			m.mu.Lock()
			m.skippedShards++
			m.mu.Unlock()
		}
	}
	return nil
}

// planStillCurrent reports whether a planned step's predecessor is still exactly what is committed.
func (m *Meter) planStillCurrent(planned forwardBatchStep) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	if !m.haveMC {
		return planned.anchor
	}
	if planned.anchor || m.lastMC+1 != planned.step.seqno {
		return false
	}
	return sameShardMap(m.lastShard, planned.previous)
}

func sameShardMap(a, b map[shardKey]uint32) bool {
	if len(a) != len(b) {
		return false
	}
	for key, seqno := range a {
		if other, ok := b[key]; !ok || other != seqno {
			return false
		}
	}
	return true
}

type fetchTarget struct {
	id   BlockID
	isMC bool
}

func (m *Meter) fetchStats(ctx context.Context, targets []fetchTarget) ([]Stat, error) {
	out, errs := m.fetchStatsResults(ctx, targets)
	for _, err := range errs {
		if err != nil {
			return nil, err
		}
	}
	return out, nil
}

// fetchStatsResults reads every target in parallel and reports each outcome positionally. The batched
// forward pass needs per-target errors: one unreadable block must cost only the steps that actually
// contain it, otherwise a single bad block anywhere in the wave would stall the whole walk.
func (m *Meter) fetchStatsResults(ctx context.Context, targets []fetchTarget) ([]Stat, []error) {
	out := make([]Stat, len(targets))
	errs := make([]error, len(targets))
	sem := make(chan struct{}, m.opts.Workers)
	var wg sync.WaitGroup
	for i, target := range targets {
		wg.Add(1)
		sem <- struct{}{}
		go func(i int, target fetchTarget) {
			defer wg.Done()
			defer func() { <-sem }()
			info, err := m.src.BlockInfo(ctx, target.id)
			if err != nil {
				errs[i] = err
				return
			}
			if info.Partial {
				errs[i] = fmt.Errorf("partial transaction page for block %+v", target.id)
				return
			}
			out[i] = Stat{ID: target.id, GenUtime: info.GenUtime, TxCount: info.TxCount, IsMC: target.isMC}
		}(i, target)
	}
	wg.Wait()
	return out, errs
}

func (m *Meter) commitForward(stats []Stat, cp Checkpoint, mcUtime int64, lineage bool) error {
	m.commitMu.Lock()
	defer m.commitMu.Unlock()
	if m.recorder != nil {
		state, err := m.recorder.CommitForward(stats, cp, mcUtime, lineage)
		if err != nil {
			return err
		}
		m.mu.Lock()
		m.appendMemoryStatsLocked(stats)
		m.applyRecorderStateLocked(state)
		m.forwardPending = false
		m.windowsReady = state.Status.CoverageStart != nil && state.Status.CoverageEnd != nil
		m.mu.Unlock()
		return nil
	}

	m.mu.Lock()
	defer m.mu.Unlock()
	m.appendMemoryStatsLocked(stats)
	if !m.coverage.HaveAnchor {
		m.coverage = CoverageState{
			HaveAnchor: true,
			Valid:      true,
			LowMC:      cp.MCSeqno,
			HighMC:     cp.MCSeqno,
			LowUtime:   mcUtime,
			HighUtime:  mcUtime,
			LowShards:  cloneShardIDs(cp.Shards),
		}
	} else {
		m.coverage.HighMC = cp.MCSeqno
		m.coverage.Valid = m.coverage.Valid && lineage
		if mcUtime > m.coverage.HighUtime {
			m.coverage.HighUtime = mcUtime
		}
	}
	m.setCheckpointLocked(cp)
	m.forwardPending = false
	return nil
}

func (m *Meter) commitBackfill(stats []Stat, low Checkpoint, mcUtime int64, lineage bool) error {
	m.commitMu.Lock()
	defer m.commitMu.Unlock()
	if m.recorder != nil {
		state, err := m.recorder.CommitBackfill(stats, low, mcUtime, lineage)
		if err != nil {
			return err
		}
		m.mu.Lock()
		m.appendMemoryStatsLocked(stats)
		m.applyRecorderStateLocked(state)
		m.backfillPending = false
		m.windowsReady = state.Status.CoverageStart != nil && state.Status.CoverageEnd != nil
		m.mu.Unlock()
		return nil
	}

	m.mu.Lock()
	defer m.mu.Unlock()
	if !m.coverage.HaveAnchor || low.MCSeqno+1 != m.coverage.LowMC {
		return fmt.Errorf("non-contiguous in-memory backfill")
	}
	m.appendMemoryStatsLocked(stats)
	m.coverage.LowMC = low.MCSeqno
	m.coverage.Valid = m.coverage.Valid && lineage
	if mcUtime < m.coverage.LowUtime {
		m.coverage.LowUtime = mcUtime
	}
	m.coverage.LowShards = cloneShardIDs(low.Shards)
	m.backfillPending = false
	return nil
}

func (m *Meter) backfill(ctx context.Context) {
	completed := false
	defer func() {
		m.mu.Lock()
		m.backfillActive = false
		m.backfillComplete = completed
		if completed {
			m.backfillPending = false
		}
		m.mu.Unlock()
	}()
	if !m.waitCoverageAnchor(ctx) {
		return
	}
	window := int64(m.opts.maxWindow())
	if window <= 0 {
		completed = true
		return
	}

	for {
		if ctx.Err() != nil {
			return
		}
		m.mu.Lock()
		coverage := cloneCoverage(m.coverage)
		target := coverage.HighUtime - window
		m.backfillActive = true
		m.backfillTarget = target
		m.backfillOldest = coverage.LowUtime + 1
		m.mu.Unlock()
		if coverage.RollbackDetected {
			return
		}
		if coverage.LowUtime+1 <= target {
			completed = true
			return
		}
		if coverage.LowMC <= 1 {
			completed = true
			return
		}

		sq := coverage.LowMC - 1
		shards, err := m.src.Shards(ctx, sq)
		if err != nil {
			m.failBackfill(err)
			if !sleepContext(ctx, 200*time.Millisecond) {
				return
			}
			continue
		}
		older, err := normalizeShardTops(shards)
		if err != nil {
			m.failBackfill(err)
			if !sleepContext(ctx, 200*time.Millisecond) {
				return
			}
			continue
		}
		identity, err := m.src.MasterchainIdentity(ctx, sq)
		if err != nil {
			m.failBackfill(fmt.Errorf("resolve backfill MC %d identity: %w", sq, err))
			if !sleepContext(ctx, 200*time.Millisecond) {
				return
			}
			continue
		}
		newer := shardMapFromIDs(coverage.LowShards)
		shardTargets, lineage := reverseTransitionTargets(older, newer)
		targets := append([]fetchTarget{{id: BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: sq}, isMC: true}}, shardTargets...)
		stats, err := m.fetchStats(ctx, targets)
		if err != nil {
			m.failBackfill(err)
			if !sleepContext(ctx, 200*time.Millisecond) {
				return
			}
			continue
		}
		mcUtime, ok := masterchainUtime(stats, sq)
		if !ok {
			m.failBackfill(fmt.Errorf("backfill batch is missing MC %d", sq))
			continue
		}
		if err := m.commitBackfill(stats, checkpointFor(sq, older, identity), mcUtime, lineage); err != nil {
			m.failBackfill(err)
			if !sleepContext(ctx, 200*time.Millisecond) {
				return
			}
			continue
		}
		if !lineage {
			m.mu.Lock()
			m.skippedShards++
			m.mu.Unlock()
		}
	}
}

func (m *Meter) waitCoverageAnchor(ctx context.Context) bool {
	for {
		m.mu.Lock()
		have := m.coverage.HaveAnchor
		verified := m.sourceVerified
		rollback := m.rollbackDetected
		bad := m.initialStateBad
		m.mu.Unlock()
		if have && verified && !rollback {
			return true
		}
		if bad || rollback {
			return false
		}
		if !sleepContext(ctx, 200*time.Millisecond) {
			return false
		}
	}
}

func transitionTargets(previous, current map[shardKey]uint32) ([]fetchTarget, bool) {
	var targets []fetchTarget
	seenTargets := map[BlockID]bool{}
	add := func(id BlockID) {
		if !seenTargets[id] {
			seenTargets[id] = true
			targets = append(targets, fetchTarget{id: id})
		}
	}
	addRange := func(key shardKey, floor, top uint32) {
		if top <= floor {
			return
		}
		for seqno := floor + 1; ; seqno++ {
			add(blockID(key, seqno))
			if seqno == top {
				break
			}
		}
	}

	valid := true
	consumed := make(map[shardKey]int, len(previous))
	splitUses := make(map[shardKey]int)
	for key, top := range current {
		if floor, ok := previous[key]; ok {
			consumed[key]++
			if top < floor {
				valid = false
				add(blockID(key, top))
			} else {
				addRange(key, floor, top)
			}
			continue
		}

		left, right, canMerge := childShardKeys(key)
		leftTop, leftOK := previous[left]
		rightTop, rightOK := previous[right]
		if canMerge && leftOK && rightOK {
			consumed[left]++
			consumed[right]++
			floor := leftTop
			if rightTop > floor {
				floor = rightTop
			}
			// Siblings do NOT have to merge from the same seqno: the merged
			// block's parents are simply each sibling's last block, and mainnet
			// merges from unequal seqnos routinely.
			if top <= floor {
				valid = false
				add(blockID(key, top))
			} else {
				addRange(key, floor, top)
			}
			continue
		}

		parent, floor, ok := immediateParentIn(previous, key)
		if ok {
			consumed[parent]++
			splitUses[parent]++
			if top <= floor {
				valid = false
				add(blockID(key, top))
			} else {
				addRange(key, floor, top)
			}
			continue
		}

		valid = false
		add(blockID(key, top))
	}

	for parent, uses := range splitUses {
		left, right, ok := childShardKeys(parent)
		if !ok || uses != 2 {
			valid = false
			continue
		}
		if _, ok := current[left]; !ok {
			valid = false
		}
		if _, ok := current[right]; !ok {
			valid = false
		}
	}
	for key := range previous {
		uses := consumed[key]
		if uses == 0 || uses > 2 || (uses == 2 && splitUses[key] != 2) {
			valid = false
		}
	}
	return targets, valid
}

func reverseTransitionTargets(older, newer map[shardKey]uint32) ([]fetchTarget, bool) {
	forward, valid := transitionTargets(older, newer)
	newerTops := make(map[BlockID]bool, len(newer))
	for key, seqno := range newer {
		newerTops[blockID(key, seqno)] = true
	}
	seen := map[BlockID]bool{}
	targets := make([]fetchTarget, 0, len(forward)+len(older))
	add := func(id BlockID) {
		if !seen[id] {
			seen[id] = true
			targets = append(targets, fetchTarget{id: id})
		}
	}
	for _, target := range forward {
		if !newerTops[target.id] {
			add(target.id)
		}
	}
	for key, seqno := range older {
		if seqno == 0 {
			continue
		}
		id := blockID(key, seqno)
		if !newerTops[id] {
			add(id)
		}
	}
	return targets, valid
}

func childShardKeys(parent shardKey) (shardKey, shardKey, bool) {
	u := uint64(parent.sh)
	bit := u & -u
	if bit <= 1 {
		return shardKey{}, shardKey{}, false
	}
	delta := bit >> 1
	return shardKey{wc: parent.wc, sh: int64(u - delta)}, shardKey{wc: parent.wc, sh: int64(u + delta)}, true
}

func immediateParentIn(shards map[shardKey]uint32, child shardKey) (shardKey, uint32, bool) {
	var found shardKey
	var seqno uint32
	matches := 0
	for candidate, top := range shards {
		left, right, ok := childShardKeys(candidate)
		if ok && (child == left || child == right) {
			found, seqno = candidate, top
			matches++
		}
	}
	return found, seqno, matches == 1
}

func normalizeShardTops(shards []BlockID) (map[shardKey]uint32, error) {
	if len(shards) == 0 {
		return nil, fmt.Errorf("masterchain block returned no shard tops")
	}
	out := make(map[shardKey]uint32, len(shards))
	for _, shard := range shards {
		key := shardKey{wc: shard.Workchain, sh: shard.Shard}
		if old, ok := out[key]; ok && old != shard.Seqno {
			return nil, fmt.Errorf("conflicting shard tops for (%d,%d): %d and %d", key.wc, key.sh, old, shard.Seqno)
		}
		out[key] = shard.Seqno
	}
	return out, nil
}

func checkpointFor(mc uint32, shards map[shardKey]uint32, identity BlockIdentity) Checkpoint {
	cp := Checkpoint{HaveMC: true, MCSeqno: mc, MCIdentity: identity, Shards: make(map[ShardID]uint32, len(shards))}
	for key, seqno := range shards {
		cp.Shards[ShardID{Workchain: key.wc, Shard: key.sh}] = seqno
	}
	return cp
}

func masterchainUtime(stats []Stat, seqno uint32) (int64, bool) {
	id := BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: seqno}
	for _, stat := range stats {
		if stat.ID == id && stat.IsMC {
			return stat.GenUtime, true
		}
	}
	return 0, false
}

func blockID(key shardKey, seqno uint32) BlockID {
	return BlockID{Workchain: key.wc, Shard: key.sh, Seqno: seqno}
}

func cloneShardMap(in map[shardKey]uint32) map[shardKey]uint32 {
	out := make(map[shardKey]uint32, len(in))
	for key, seqno := range in {
		out[key] = seqno
	}
	return out
}

func shardMapFromIDs(in map[ShardID]uint32) map[shardKey]uint32 {
	out := make(map[shardKey]uint32, len(in))
	for key, seqno := range in {
		out[shardKey{wc: key.Workchain, sh: key.Shard}] = seqno
	}
	return out
}

func cloneCoverage(in CoverageState) CoverageState {
	in.LowShards = cloneShardIDs(in.LowShards)
	return in
}

func sleepContext(ctx context.Context, duration time.Duration) bool {
	select {
	case <-ctx.Done():
		return false
	case <-time.After(duration):
		return true
	}
}

func (m *Meter) applyRecorderStateLocked(state RecorderState) {
	m.setCheckpointLocked(state.Checkpoint)
	m.coverage = cloneCoverage(state.Coverage)
	m.storedBlocks = state.Status.Blocks
	m.partialBlocks = state.Status.PartialBlocks
	m.rollbackDetected = state.Status.RollbackDetected
	if state.Coverage.HaveAnchor && state.Coverage.HighUtime > m.maxUtime {
		m.maxUtime = state.Coverage.HighUtime
	}
}

func (m *Meter) setCheckpointLocked(cp Checkpoint) {
	m.haveMC = cp.HaveMC
	m.lastMC = cp.MCSeqno
	m.lastMCIdentity = cp.MCIdentity
	m.lastShard = shardMapFromIDs(cp.Shards)
	m.activeShards = make(map[shardKey]bool, len(m.lastShard))
	for key := range m.lastShard {
		m.activeShards[key] = true
	}
}

func (m *Meter) markChainInvalid(err error) {
	m.mu.Lock()
	m.sourceVerified = false
	m.forwardSynced = false
	m.mu.Unlock()
	m.commitMu.Lock()
	if m.recorder != nil {
		state, markErr := m.recorder.MarkRollback()
		if markErr != nil {
			err = fmt.Errorf("%v; %w", err, markErr)
		} else {
			m.mu.Lock()
			m.applyRecorderStateLocked(state)
			m.mu.Unlock()
		}
	} else {
		m.mu.Lock()
		m.coverage.Valid = false
		m.coverage.RollbackDetected = true
		m.rollbackDetected = true
		m.mu.Unlock()
	}
	m.commitMu.Unlock()
	m.failForward(err)
}

func (m *Meter) failForward(err error) {
	m.mu.Lock()
	m.forwardPending = true
	m.forwardSynced = false
	m.mu.Unlock()
	m.recordErr(err)
}

func (m *Meter) failBackfill(err error) {
	m.mu.Lock()
	m.backfillPending = true
	m.mu.Unlock()
	m.recordErr(err)
}

func (m *Meter) countPoll() {
	m.mu.Lock()
	m.polls++
	m.mu.Unlock()
}

func (m *Meter) recordErr(err error) {
	if err == nil {
		return
	}
	m.mu.Lock()
	m.errCount++
	m.lastErr = err.Error()
	m.mu.Unlock()
}

// appendStat is retained for synthetic in-memory tests and direct low-level
// recording. It never establishes canonical coverage.
func (m *Meter) appendStat(stat Stat) error {
	zeroID := stat.ID == (BlockID{})
	if !zeroID && m.recorder != nil {
		blockRecorder, ok := m.recorder.(interface {
			RecordBlock(Stat) (bool, error)
		})
		if !ok {
			return fmt.Errorf("recorder does not support individual blocks")
		}
		inserted, err := blockRecorder.RecordBlock(stat)
		if err != nil {
			m.recordErr(err)
			return err
		}
		if !inserted {
			return nil
		}
		m.mu.Lock()
		m.storedBlocks++
		m.appendMemoryStatLocked(stat)
		m.mu.Unlock()
		return nil
	}
	m.mu.Lock()
	m.appendMemoryStatLocked(stat)
	m.mu.Unlock()
	return nil
}

func (m *Meter) appendMemoryStatsLocked(stats []Stat) {
	for _, stat := range stats {
		m.appendMemoryStatLocked(stat)
	}
}

func (m *Meter) appendMemoryStatLocked(stat Stat) {
	if stat.ID != (BlockID{}) {
		if m.seen[stat.ID] {
			return
		}
		m.seen[stat.ID] = true
	}
	if stat.GenUtime > m.maxUtime {
		m.maxUtime = stat.GenUtime
	}
	if stat.Partial {
		m.partialBlocks++
	}
	entry := historyEntry{ID: stat.ID, GenUtime: stat.GenUtime, TxCount: stat.TxCount}
	if stat.IsMC {
		entry.MCBlocks = 1
	} else {
		entry.WCBlocks = 1
	}
	m.history = append(m.history, entry)
	if len(m.history) <= m.opts.HistoryMax {
		return
	}
	n := m.opts.HistoryMax
	for _, old := range m.history[:len(m.history)-n] {
		if old.ID != (BlockID{}) {
			delete(m.seen, old.ID)
		}
	}
	m.history = append(m.history[:0], m.history[len(m.history)-n:]...)
}

func (m *Meter) hydrateRecentLocked(points []SecondStat) {
	m.history = make([]historyEntry, 0, len(points))
	for _, point := range points {
		m.history = append(m.history, historyEntry{
			GenUtime: point.GenUtime,
			TxCount:  point.TxCount,
			MCBlocks: point.MCBlocks,
			WCBlocks: point.WCBlocks,
		})
	}
}

func (m *Meter) TPS(windowSec int) WindowTPS {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.tpsLocked(windowSec)
}

func (m *Meter) tpsLocked(windowSec int) WindowTPS {
	out := WindowTPS{WindowSec: windowSec}
	if len(m.history) == 0 {
		return out
	}
	now, minUtime := m.history[0].GenUtime, m.history[0].GenUtime
	for _, block := range m.history {
		if block.GenUtime > now {
			now = block.GenUtime
		}
		if block.GenUtime < minUtime {
			minUtime = block.GenUtime
		}
	}
	cutoff := now - int64(windowSec)
	for _, block := range m.history {
		if block.GenUtime > cutoff {
			out.TotalTx += block.TxCount
			out.MCBlocks += block.MCBlocks
			out.WCBlocks += block.WCBlocks
		}
	}
	out.Blocks = out.MCBlocks + out.WCBlocks
	out.TPS = float64(out.TotalTx) / float64(windowSec)
	out.MCBlockRate = float64(out.MCBlocks) / float64(windowSec)
	out.WCBlockRate = float64(out.WCBlocks) / float64(windowSec)
	out.BlockRate = float64(out.Blocks) / float64(windowSec)
	out.Filling = now-minUtime < int64(windowSec)
	return out
}

func (m *Meter) ShardCount() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.activeShards)
}

func (m *Meter) Snapshot() Snapshot {
	m.mu.Lock()
	defer m.mu.Unlock()
	snapshot := Snapshot{
		Shards:              len(m.activeShards),
		Polls:               m.polls,
		Errors:              m.errCount,
		SkippedMC:           m.skippedMC,
		SkippedShards:       m.skippedShards,
		PartialBlocks:       m.partialBlocks,
		StoredBlocks:        m.storedBlocks,
		CoverageValid:       m.coverage.HaveAnchor && m.coverage.Valid && !m.coverage.RollbackDetected,
		RollbackDetected:    m.rollbackDetected,
		PendingBatch:        m.forwardPending || m.backfillPending,
		SourceVerified:      m.sourceVerified,
		ForwardSynced:       m.forwardSynced,
		WindowsReady:        m.windowsReady,
		LastErr:             m.lastErr,
		BackfillActive:      m.backfillActive,
		BackfillComplete:    m.backfillComplete,
		BackfillOldestUtime: m.backfillOldest,
		BackfillTargetUtime: m.backfillTarget,
		MaxUtime:            m.maxUtime,
	}
	for _, window := range m.opts.Windows {
		snapshot.Windows = append(snapshot.Windows, m.tpsLocked(window))
	}
	if len(m.history) > 0 {
		now, minUtime := m.history[0].GenUtime, m.history[0].GenUtime
		for _, block := range m.history {
			if block.GenUtime > now {
				now = block.GenUtime
			}
			if block.GenUtime < minUtime {
				minUtime = block.GenUtime
			}
			snapshot.TotalTx += block.TxCount
			snapshot.MCBlocks += block.MCBlocks
			snapshot.WCBlocks += block.WCBlocks
		}
		snapshot.Blocks = snapshot.MCBlocks + snapshot.WCBlocks
		snapshot.MaxUtime = now
		snapshot.SpanSec = int(now - minUtime)
	}
	return snapshot
}

func (m *Meter) Uptime() time.Duration { return time.Since(m.started) }

func (snapshot Snapshot) String() string {
	line := fmt.Sprintf("shards=%d", snapshot.Shards)
	for _, window := range snapshot.Windows {
		mark := ""
		if window.Filling {
			mark = "*"
		}
		line += fmt.Sprintf("  %s=%.1f%s", windowLabel(window.WindowSec), window.TPS, mark)
	}
	if len(snapshot.Windows) > 0 {
		short := snapshot.Windows[0]
		line += fmt.Sprintf("  blk/s[%s] mc=%.2f wc=%.2f", windowLabel(short.WindowSec), short.MCBlockRate, short.WCBlockRate)
		if snapshot.Shards > 0 && short.WCBlockRate > 0 {
			line += fmt.Sprintf(" (%.0fms/shard)", 1000*float64(snapshot.Shards)/short.WCBlockRate)
		}
	}
	line += fmt.Sprintf("   (span %ds, %d tx, poll %d, err %d)", snapshot.SpanSec, snapshot.TotalTx, snapshot.Polls, snapshot.Errors)
	if snapshot.PartialBlocks != 0 || snapshot.SkippedMC != 0 || snapshot.SkippedShards != 0 || snapshot.RollbackDetected {
		line += fmt.Sprintf("  [quality partial=%d mc-skip=%d shard-skip=%d rollback=%t]", snapshot.PartialBlocks, snapshot.SkippedMC, snapshot.SkippedShards, snapshot.RollbackDetected)
	}
	if snapshot.BackfillActive {
		reached := snapshot.MaxUtime - snapshot.BackfillOldestUtime
		total := snapshot.MaxUtime - snapshot.BackfillTargetUtime
		if total < 1 {
			total = 1
		}
		line += fmt.Sprintf("  [catchup %ds/%ds]", reached, total)
	}
	return line
}

func windowLabel(sec int) string {
	switch sec {
	case 30:
		return "30s"
	case 60:
		return "1m"
	case 300:
		return "5m"
	case 900:
		return "15m"
	default:
		if sec%60 == 0 {
			return fmt.Sprintf("%dm", sec/60)
		}
		return fmt.Sprintf("%ds", sec)
	}
}
