package tpsmeter

// Recorder atomically commits canonical blocks together with the traversal
// frontier that proves which time range is complete.
type Recorder interface {
	LoadState() (RecorderState, error)
	CommitForward([]Stat, Checkpoint, int64, bool) (RecorderState, error)
	CommitBackfill([]Stat, Checkpoint, int64, bool) (RecorderState, error)
	MarkRollback() (RecorderState, error)
	LoadRecentSeconds(start, end int64) ([]SecondStat, error)
	Status() (StoreStatus, error)
}

// ShardID identifies a shard independently of its block seqno.
type ShardID struct {
	Workchain int32
	Shard     int64
}

// Checkpoint is the last fully recorded forward-poll position.
type Checkpoint struct {
	HaveMC     bool
	MCSeqno    uint32
	MCIdentity BlockIdentity
	Shards     map[ShardID]uint32
}

// SecondStat is one exact persisted per-second aggregate for live window reload.
type SecondStat struct {
	GenUtime      int64
	TxCount       int
	MCBlocks      int
	WCBlocks      int
	PartialBlocks uint64
}

// CoverageState is a contiguous MC traversal frontier. LowShards is the active
// shard-top set at LowMC and is the durable restart point for backward traversal.
type CoverageState struct {
	HaveAnchor       bool
	Valid            bool
	RollbackDetected bool
	LowMC            uint32
	HighMC           uint32
	LowUtime         int64
	HighUtime        int64
	LowShards        map[ShardID]uint32
}

// RecorderState is loaded on startup and returned after every atomic commit.
type RecorderState struct {
	Checkpoint Checkpoint
	Coverage   CoverageState
	Status     StoreStatus
}
