package tpsmeter

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"path/filepath"
	"sort"
	"time"

	bolt "go.etcd.io/bbolt"
)

const (
	storeSchemaVersion uint64 = 4
	firstStoreEpoch    uint64 = 1
)

var (
	blocksBucket  = []byte("blocks")
	secondsBucket = []byte("seconds")
	shardsBucket  = []byte("shards")
	metaBucket    = []byte("meta")
	epochsBucket  = []byte("epochs")

	schemaKey           = []byte("schema")
	activeEpochKey      = []byte("active_epoch")
	blockCountKey       = []byte("block_count")
	partialCountKey     = []byte("partial_count")
	checkpointKey       = []byte("checkpoint")
	coverageKey         = []byte("coverage")
	lastMismatchKey     = []byte("last_mismatch_at")
	errInvalidStoreData = errors.New("invalid tpsmeter store data")
)

// Store is a durable bbolt-backed block recorder and aggregate query store.
type Store struct {
	db   *bolt.DB
	path string
}

// StoreStatus describes durable contiguous traversal coverage for the active
// epoch. CoverageEnd is exclusive. Bounds are omitted until at least one
// non-empty second is proven.
type StoreStatus struct {
	Epoch            uint64     `json:"epoch"`
	Blocks           uint64     `json:"blocks"`
	PartialBlocks    uint64     `json:"partial_blocks"`
	CoverageValid    bool       `json:"coverage_valid"`
	RollbackDetected bool       `json:"rollback_detected"`
	LastMismatchAt   *time.Time `json:"last_mismatch_at,omitempty"`
	CoverageStart    *time.Time `json:"coverage_start"`
	CoverageEnd      *time.Time `json:"coverage_end"`
}

// OpenStore opens or creates a multi-run bbolt history store at path.
func OpenStore(path string) (*Store, error) {
	if path == "" {
		return nil, fmt.Errorf("store path is empty")
	}
	db, err := bolt.Open(path, 0o600, &bolt.Options{Timeout: time.Second})
	if err != nil {
		return nil, fmt.Errorf("open store %s: %w", path, err)
	}
	s := &Store{db: db, path: path}
	if err := db.Update(func(tx *bolt.Tx) error {
		meta := tx.Bucket(metaBucket)
		if meta == nil {
			if tx.Bucket(blocksBucket) != nil || tx.Bucket(secondsBucket) != nil || tx.Bucket(epochsBucket) != nil {
				return fmt.Errorf("unversioned store has data buckets")
			}
			var err error
			meta, err = tx.CreateBucket(metaBucket)
			if err != nil {
				return err
			}
			return initializeV4Store(tx, meta)
		}

		raw := meta.Get(schemaKey)
		if raw == nil {
			if tx.Bucket(blocksBucket) != nil || tx.Bucket(secondsBucket) != nil || tx.Bucket(epochsBucket) != nil || meta.Stats().KeyN != 0 {
				return fmt.Errorf("unversioned store is not empty")
			}
			return initializeV4Store(tx, meta)
		}
		version, err := decodeUint64(raw)
		if err != nil {
			return fmt.Errorf("schema: %w", err)
		}
		switch version {
		case 3:
			return migrateV3Store(tx, meta)
		case storeSchemaVersion:
			if tx.Bucket(blocksBucket) != nil || tx.Bucket(secondsBucket) != nil {
				return fmt.Errorf("invalid schema v4 bucket layout")
			}
			_, err := activeEpochData(tx)
			return err
		default:
			return fmt.Errorf("unsupported store schema %d", version)
		}
	}); err != nil {
		db.Close()
		return nil, fmt.Errorf("initialize store %s: %w", path, err)
	}
	return s, nil
}

type epochData struct {
	id      uint64
	bucket  *bolt.Bucket
	blocks  *bolt.Bucket
	seconds *bolt.Bucket
	shards  *bolt.Bucket
	meta    *bolt.Bucket
}

func initializeV4Store(tx *bolt.Tx, meta *bolt.Bucket) error {
	epochs, err := tx.CreateBucket(epochsBucket)
	if err != nil {
		return err
	}
	if _, err := createEpochData(epochs, firstStoreEpoch); err != nil {
		return err
	}
	if err := putUint64(meta, activeEpochKey, firstStoreEpoch); err != nil {
		return err
	}
	return putUint64(meta, schemaKey, storeSchemaVersion)
}

func migrateV3Store(tx *bolt.Tx, global *bolt.Bucket) error {
	legacyBlocks := tx.Bucket(blocksBucket)
	legacySeconds := tx.Bucket(secondsBucket)
	if legacyBlocks == nil || legacySeconds == nil || tx.Bucket(epochsBucket) != nil {
		return fmt.Errorf("invalid schema v3 bucket layout")
	}
	epochs, err := tx.CreateBucket(epochsBucket)
	if err != nil {
		return err
	}
	epoch, err := createEpochData(epochs, firstStoreEpoch)
	if err != nil {
		return err
	}
	if err := copyBucket(epoch.blocks, legacyBlocks); err != nil {
		return fmt.Errorf("migrate v3 blocks: %w", err)
	}
	if err := copyBucket(epoch.seconds, legacySeconds); err != nil {
		return fmt.Errorf("migrate v3 seconds: %w", err)
	}

	var movedKeys [][]byte
	if err := global.ForEach(func(key, value []byte) error {
		if bytes.Equal(key, schemaKey) {
			return nil
		}
		if value == nil {
			return fmt.Errorf("invalid nested bucket in v3 metadata")
		}
		if err := epoch.meta.Put(key, value); err != nil {
			return err
		}
		movedKeys = append(movedKeys, append([]byte(nil), key...))
		return nil
	}); err != nil {
		return fmt.Errorf("migrate v3 metadata: %w", err)
	}
	for _, key := range movedKeys {
		if err := global.Delete(key); err != nil {
			return err
		}
	}
	if err := tx.DeleteBucket(blocksBucket); err != nil {
		return err
	}
	if err := tx.DeleteBucket(secondsBucket); err != nil {
		return err
	}
	if err := putUint64(global, activeEpochKey, firstStoreEpoch); err != nil {
		return err
	}
	return putUint64(global, schemaKey, storeSchemaVersion)
}

func copyBucket(dst, src *bolt.Bucket) error {
	return src.ForEach(func(key, value []byte) error {
		if value == nil {
			return errInvalidStoreData
		}
		return dst.Put(key, value)
	})
}

func createEpochData(epochs *bolt.Bucket, id uint64) (epochData, error) {
	if id == 0 {
		return epochData{}, errInvalidStoreData
	}
	epoch, err := epochs.CreateBucket(encodeEpoch(id))
	if err != nil {
		return epochData{}, err
	}
	for _, name := range [][]byte{blocksBucket, secondsBucket, shardsBucket, metaBucket} {
		if _, err := epoch.CreateBucket(name); err != nil {
			return epochData{}, err
		}
	}
	return epochDataFromBucket(id, epoch)
}

func activeEpochData(tx *bolt.Tx) (epochData, error) {
	global := tx.Bucket(metaBucket)
	if global == nil {
		return epochData{}, errInvalidStoreData
	}
	id, err := readRequiredUint64(global, activeEpochKey)
	if err != nil {
		return epochData{}, fmt.Errorf("active epoch: %w", err)
	}
	return epochDataByID(tx, id)
}

func epochDataByID(tx *bolt.Tx, id uint64) (epochData, error) {
	epochs := tx.Bucket(epochsBucket)
	if epochs == nil || id == 0 {
		return epochData{}, errInvalidStoreData
	}
	epoch := epochs.Bucket(encodeEpoch(id))
	if epoch == nil {
		return epochData{}, errInvalidStoreData
	}
	return epochDataFromBucket(id, epoch)
}

func epochDataFromBucket(id uint64, epoch *bolt.Bucket) (epochData, error) {
	if epoch == nil {
		return epochData{}, errInvalidStoreData
	}
	data := epochData{
		id:      id,
		bucket:  epoch,
		blocks:  epoch.Bucket(blocksBucket),
		seconds: epoch.Bucket(secondsBucket),
		shards:  epoch.Bucket(shardsBucket),
		meta:    epoch.Bucket(metaBucket),
	}
	if data.blocks == nil || data.seconds == nil || data.meta == nil {
		return epochData{}, errInvalidStoreData
	}
	return data, nil
}

// ResetLiveChain closes a non-empty active epoch and starts a fresh one. All
// recorded blocks and aggregates remain queryable in their original epoch.
func (s *Store) ResetLiveChain() error {
	var oldEpoch uint64
	err := s.db.Update(func(tx *bolt.Tx) error {
		active, err := activeEpochData(tx)
		if err != nil {
			return err
		}
		oldEpoch = active.id
		empty, err := epochIsEmpty(active)
		if err != nil {
			return err
		}
		if empty {
			return nil
		}
		epochs := tx.Bucket(epochsBucket)
		newEpoch, err := nextEpochID(epochs)
		if err != nil {
			return err
		}
		if _, err := createEpochData(epochs, newEpoch); err != nil {
			return err
		}
		return putUint64(tx.Bucket(metaBucket), activeEpochKey, newEpoch)
	})
	if err != nil {
		return fmt.Errorf("reset live chain at epoch %d: %w", oldEpoch, err)
	}
	return nil
}

// ImportStore copies every non-empty epoch from another history database into
// this store without changing the active epoch. The source may use schema v3;
// OpenStore migrates it before the atomic copy.
func (s *Store) ImportStore(path string) ([]uint64, error) {
	if filepath.Clean(path) == filepath.Clean(s.path) {
		return nil, fmt.Errorf("cannot import a store into itself")
	}
	source, err := OpenStore(path)
	if err != nil {
		return nil, fmt.Errorf("open import store: %w", err)
	}
	defer source.Close()

	var imported []uint64
	err = source.db.View(func(sourceTx *bolt.Tx) error {
		sourceEpochs := sourceTx.Bucket(epochsBucket)
		if sourceEpochs == nil {
			return errInvalidStoreData
		}
		return s.db.Update(func(destinationTx *bolt.Tx) error {
			destinationEpochs := destinationTx.Bucket(epochsBucket)
			if destinationEpochs == nil {
				return errInvalidStoreData
			}
			cursor := sourceEpochs.Cursor()
			for key, _ := cursor.First(); key != nil; key, _ = cursor.Next() {
				sourceID, err := decodeEpoch(key)
				if err != nil {
					return err
				}
				sourceEpoch, err := epochDataFromBucket(sourceID, sourceEpochs.Bucket(key))
				if err != nil {
					return err
				}
				empty, err := epochIsEmpty(sourceEpoch)
				if err != nil {
					return err
				}
				if empty {
					continue
				}
				destinationID, err := nextEpochID(destinationEpochs)
				if err != nil {
					return err
				}
				destinationEpoch, err := createEpochData(destinationEpochs, destinationID)
				if err != nil {
					return err
				}
				for _, buckets := range [][2]*bolt.Bucket{
					{destinationEpoch.blocks, sourceEpoch.blocks},
					{destinationEpoch.seconds, sourceEpoch.seconds},
					{destinationEpoch.meta, sourceEpoch.meta},
				} {
					if err := copyBucket(buckets[0], buckets[1]); err != nil {
						return err
					}
				}
				if sourceEpoch.shards != nil {
					if err := copyBucket(destinationEpoch.shards, sourceEpoch.shards); err != nil {
						return err
					}
				}
				imported = append(imported, destinationID)
			}
			return nil
		})
	})
	if err != nil {
		return nil, fmt.Errorf("import store %s: %w", path, err)
	}
	return imported, nil
}

func nextEpochID(epochs *bolt.Bucket) (uint64, error) {
	if epochs == nil {
		return 0, errInvalidStoreData
	}
	key, _ := epochs.Cursor().Last()
	if key == nil {
		return firstStoreEpoch, nil
	}
	last, err := decodeEpoch(key)
	if err != nil {
		return 0, err
	}
	if last == math.MaxUint64 {
		return 0, fmt.Errorf("store epoch overflow")
	}
	return last + 1, nil
}

func epochIsEmpty(epoch epochData) (bool, error) {
	if key, _ := epoch.blocks.Cursor().First(); key != nil {
		return false, nil
	}
	if key, _ := epoch.seconds.Cursor().First(); key != nil {
		return false, nil
	}
	if epoch.shards != nil {
		if key, _ := epoch.shards.Cursor().First(); key != nil {
			return false, nil
		}
	}
	err := epoch.meta.ForEach(func(key, value []byte) error {
		if value == nil {
			return errInvalidStoreData
		}
		if bytes.Equal(key, blockCountKey) || bytes.Equal(key, partialCountKey) {
			count, err := decodeUint64(value)
			if err != nil {
				return err
			}
			if count == 0 {
				return nil
			}
		}
		return errEpochNotEmpty
	})
	if errors.Is(err, errEpochNotEmpty) {
		return false, nil
	}
	return err == nil, err
}

var errEpochNotEmpty = errors.New("epoch is not empty")

// Path returns the backing file path.
func (s *Store) Path() string { return s.path }

// Close flushes and closes the store.
func (s *Store) Close() error {
	if s == nil || s.db == nil {
		return nil
	}
	return s.db.Close()
}

// RecordBlock inserts one block without changing canonical coverage. It is useful
// for synthetic/query data; Meter uses the atomic Commit methods below.
func (s *Store) RecordBlock(stat Stat) (bool, error) {
	inserted := false
	err := s.db.Update(func(tx *bolt.Tx) error {
		epoch, err := activeEpochData(tx)
		if err != nil {
			return err
		}
		inserted, err = insertStat(epoch, stat)
		return err
	})
	if err != nil {
		return false, fmt.Errorf("record block %+v: %w", stat.ID, err)
	}
	return inserted, nil
}

// Block loads the newest-epoch copy of a block ID.
func (s *Store) Block(id BlockID) (Stat, bool, error) {
	var stat Stat
	found := false
	err := s.db.View(func(tx *bolt.Tx) error {
		active, err := activeEpochData(tx)
		if err != nil {
			return err
		}
		if raw := active.blocks.Get(encodeBlockID(id)); raw != nil {
			stat, err = decodeStat(id, raw)
			if err == nil {
				found = true
			}
			return err
		}
		epochs := tx.Bucket(epochsBucket)
		if epochs == nil {
			return errInvalidStoreData
		}
		cursor := epochs.Cursor()
		for key, _ := cursor.Last(); key != nil; key, _ = cursor.Prev() {
			epochID, err := decodeEpoch(key)
			if err != nil {
				return err
			}
			if epochID == active.id {
				continue
			}
			epoch, err := epochDataFromBucket(epochID, epochs.Bucket(key))
			if err != nil {
				return err
			}
			raw := epoch.blocks.Get(encodeBlockID(id))
			if raw == nil {
				continue
			}
			stat, err = decodeStat(id, raw)
			if err == nil {
				found = true
			}
			return err
		}
		return nil
	})
	if err != nil {
		return Stat{}, false, fmt.Errorf("read block %+v: %w", id, err)
	}
	return stat, found, nil
}

// BlockAtEpoch loads a block from one specific store epoch.
func (s *Store) BlockAtEpoch(epochID uint64, id BlockID) (Stat, bool, error) {
	var stat Stat
	found := false
	err := s.db.View(func(tx *bolt.Tx) error {
		epoch, err := epochDataByID(tx, epochID)
		if err != nil {
			return err
		}
		raw := epoch.blocks.Get(encodeBlockID(id))
		if raw == nil {
			return nil
		}
		stat, err = decodeStat(id, raw)
		if err == nil {
			found = true
		}
		return err
	})
	if err != nil {
		return Stat{}, false, fmt.Errorf("read block %+v at epoch %d: %w", id, epochID, err)
	}
	return stat, found, nil
}

// LoadRecentSeconds loads active-epoch aggregate points for every second in
// [start,end), including zero-valued seconds so live window state is preserved.
func (s *Store) LoadRecentSeconds(start, end int64) ([]SecondStat, error) {
	if end <= start {
		return []SecondStat{}, nil
	}
	duration := end - start
	if duration > int64(math.MaxInt) {
		return nil, fmt.Errorf("recent second range is too large")
	}
	points := make([]SecondStat, int(duration))
	for i := range points {
		points[i].GenUtime = start + int64(i)
	}
	err := s.db.View(func(tx *bolt.Tx) error {
		epoch, err := activeEpochData(tx)
		if err != nil {
			return err
		}
		cursor := epoch.seconds.Cursor()
		for key, value := cursor.Seek(encodeSecond(start)); key != nil; key, value = cursor.Next() {
			second, err := decodeSecond(key)
			if err != nil {
				return err
			}
			if second >= end {
				break
			}
			aggregate, err := decodeAggregate(value)
			if err != nil {
				return err
			}
			if math.MaxUint64-aggregate.MCTx < aggregate.WCTx || math.MaxUint64-aggregate.MCBlocks < aggregate.WCBlocks {
				return fmt.Errorf("aggregate overflow at %d", second)
			}
			totalTx := aggregate.MCTx + aggregate.WCTx
			blocks := aggregate.MCBlocks + aggregate.WCBlocks
			if totalTx > uint64(math.MaxInt) || blocks > uint64(math.MaxInt) {
				return fmt.Errorf("aggregate exceeds platform int at %d", second)
			}
			points[second-start].TxCount = int(totalTx)
			points[second-start].MCBlocks = int(aggregate.MCBlocks)
			points[second-start].WCBlocks = int(aggregate.WCBlocks)
			points[second-start].PartialBlocks = aggregate.PartialBlocks
		}
		return nil
	})
	if err != nil {
		return nil, fmt.Errorf("load recent seconds: %w", err)
	}
	return points, nil
}

// Status returns active-epoch durable block counts and coverage.
func (s *Store) Status() (StoreStatus, error) {
	var status StoreStatus
	err := s.db.View(func(tx *bolt.Tx) error {
		epoch, err := activeEpochData(tx)
		if err != nil {
			return err
		}
		status, err = storeStatus(epoch)
		return err
	})
	if err != nil {
		return StoreStatus{}, fmt.Errorf("read store status: %w", err)
	}
	return status, nil
}

// LoadState returns the active epoch's checkpoint and coverage frontier.
func (s *Store) LoadState() (RecorderState, error) {
	var state RecorderState
	err := s.db.View(func(tx *bolt.Tx) error {
		epoch, err := activeEpochData(tx)
		if err != nil {
			return err
		}
		state, err = recorderState(epoch)
		return err
	})
	if err != nil {
		return RecorderState{}, fmt.Errorf("load recorder state: %w", err)
	}
	return state, nil
}

// LoadCheckpoint returns the last atomically committed forward checkpoint.
func (s *Store) LoadCheckpoint() (Checkpoint, error) {
	state, err := s.LoadState()
	if err != nil {
		return Checkpoint{}, err
	}
	return state.Checkpoint, nil
}

// CommitForward atomically records one fully fetched MC transition, replaces the
// active-shard checkpoint, and advances the high coverage frontier.
func (s *Store) CommitForward(stats []Stat, cp Checkpoint, mcUtime int64, lineage bool) (RecorderState, error) {
	if err := validateCanonicalBatch(stats, cp.MCSeqno, mcUtime); err != nil {
		return RecorderState{}, err
	}
	var state RecorderState
	err := s.db.Update(func(tx *bolt.Tx) error {
		epoch, err := activeEpochData(tx)
		if err != nil {
			return err
		}
		old, err := recorderState(epoch)
		if err != nil {
			return err
		}
		if !cp.HaveMC {
			return fmt.Errorf("forward checkpoint has no masterchain block")
		}
		if cp.MCIdentity.isZero() {
			return fmt.Errorf("forward checkpoint MC identity is missing")
		}
		if old.Checkpoint.HaveMC && cp.MCSeqno != old.Checkpoint.MCSeqno+1 {
			return fmt.Errorf("non-contiguous forward checkpoint %d after %d", cp.MCSeqno, old.Checkpoint.MCSeqno)
		}
		if _, err := insertStats(epoch, stats); err != nil {
			return err
		}
		if err := writeShardSample(epoch, mcUtime, cp); err != nil {
			return err
		}
		lowBoundary, highBoundary, err := frontierBounds(epoch, cp, mcUtime)
		if err != nil {
			return err
		}

		coverage := old.Coverage
		if !coverage.HaveAnchor {
			coverage = CoverageState{
				HaveAnchor: true,
				Valid:      true,
				LowMC:      cp.MCSeqno,
				HighMC:     cp.MCSeqno,
				LowUtime:   lowBoundary,
				HighUtime:  highBoundary,
				LowShards:  cloneShardIDs(cp.Shards),
			}
		} else {
			if coverage.HighMC != old.Checkpoint.MCSeqno {
				return fmt.Errorf("coverage high %d does not match checkpoint %d", coverage.HighMC, old.Checkpoint.MCSeqno)
			}
			coverage.HighMC = cp.MCSeqno
			coverage.Valid = coverage.Valid && lineage
			if highBoundary > coverage.HighUtime {
				coverage.HighUtime = highBoundary
			}
		}
		if err := writeCheckpoint(epoch, cp); err != nil {
			return err
		}
		if err := writeCoverage(epoch, coverage); err != nil {
			return err
		}
		state, err = recorderState(epoch)
		return err
	})
	if err != nil {
		return RecorderState{}, fmt.Errorf("commit forward MC %d: %w", cp.MCSeqno, err)
	}
	return state, nil
}

// BackfillStep is one adjacent older MC transition: the canonical blocks it adds
// and the traversal frontier they prove.
type BackfillStep struct {
	Stats      []Stat
	Checkpoint Checkpoint
	MCUtime    int64
	Lineage    bool
}

// CommitBackfill atomically records one adjacent older MC transition and lowers
// the coverage frontier. The forward checkpoint is intentionally unchanged.
func (s *Store) CommitBackfill(stats []Stat, low Checkpoint, mcUtime int64, lineage bool) (RecorderState, error) {
	return s.CommitBackfillBatch([]BackfillStep{{Stats: stats, Checkpoint: low, MCUtime: mcUtime, Lineage: lineage}})
}

// CommitBackfillBatch applies many descending backfill steps in ONE transaction.
// Steps must be strictly descending and adjacent, exactly as CommitBackfill
// requires them one at a time; the batch exists so a bulk historical import is
// not bounded by one fsync per masterchain block.
func (s *Store) CommitBackfillBatch(steps []BackfillStep) (RecorderState, error) {
	if len(steps) == 0 {
		return RecorderState{}, fmt.Errorf("backfill batch is empty")
	}
	for _, step := range steps {
		if err := validateCanonicalBatch(step.Stats, step.Checkpoint.MCSeqno, step.MCUtime); err != nil {
			return RecorderState{}, err
		}
	}
	var state RecorderState
	var failed uint32
	err := s.db.Update(func(tx *bolt.Tx) error {
		epoch, err := activeEpochData(tx)
		if err != nil {
			return err
		}
		old, err := recorderState(epoch)
		if err != nil {
			return err
		}
		coverage := old.Coverage
		for _, step := range steps {
			low := step.Checkpoint
			failed = low.MCSeqno
			if !low.HaveMC || !coverage.HaveAnchor {
				return fmt.Errorf("backfill has no coverage anchor")
			}
			if low.MCIdentity.isZero() {
				return fmt.Errorf("backfill checkpoint MC identity is missing")
			}
			if low.MCSeqno+1 != coverage.LowMC {
				return fmt.Errorf("non-contiguous backfill checkpoint %d before %d", low.MCSeqno, coverage.LowMC)
			}
			if _, err := insertStats(epoch, step.Stats); err != nil {
				return err
			}
			if err := writeShardSample(epoch, step.MCUtime, low); err != nil {
				return err
			}
			lowBoundary, _, err := frontierBounds(epoch, low, step.MCUtime)
			if err != nil {
				return err
			}

			coverage.LowMC = low.MCSeqno
			coverage.Valid = coverage.Valid && step.Lineage
			if lowBoundary < coverage.LowUtime {
				coverage.LowUtime = lowBoundary
			}
			coverage.LowShards = cloneShardIDs(low.Shards)
		}
		if err := writeCoverage(epoch, coverage); err != nil {
			return err
		}
		state, err = recorderState(epoch)
		return err
	})
	if err != nil {
		return RecorderState{}, fmt.Errorf("commit backfill MC %d: %w", failed, err)
	}
	return state, nil
}

// MarkRollback durably invalidates coverage after observing a chain top below
// the stored forward checkpoint.
func (s *Store) MarkRollback() (RecorderState, error) {
	var state RecorderState
	err := s.db.Update(func(tx *bolt.Tx) error {
		epoch, err := activeEpochData(tx)
		if err != nil {
			return err
		}
		old, err := recorderState(epoch)
		if err != nil {
			return err
		}
		old.Coverage.Valid = false
		old.Coverage.RollbackDetected = true
		if err := putUint64(epoch.meta, lastMismatchKey, uint64(time.Now().Unix())); err != nil {
			return err
		}
		if err := writeCoverage(epoch, old.Coverage); err != nil {
			return err
		}
		state, err = recorderState(epoch)
		return err
	})
	if err != nil {
		return RecorderState{}, fmt.Errorf("mark chain rollback: %w", err)
	}
	return state, nil
}

type secondAggregate struct {
	MCTx          uint64
	WCTx          uint64
	MCBlocks      uint64
	WCBlocks      uint64
	PartialBlocks uint64
}

func storeStatus(epoch epochData) (StoreStatus, error) {
	blocks, err := readUint64(epoch.meta, blockCountKey)
	if err != nil {
		return StoreStatus{}, err
	}
	partials, err := readUint64(epoch.meta, partialCountKey)
	if err != nil {
		return StoreStatus{}, err
	}
	coverage, err := readCoverage(epoch)
	if err != nil {
		return StoreStatus{}, err
	}
	lastMismatch, err := readUint64(epoch.meta, lastMismatchKey)
	if err != nil {
		return StoreStatus{}, err
	}
	status := StoreStatus{
		Epoch:            epoch.id,
		Blocks:           blocks,
		PartialBlocks:    partials,
		CoverageValid:    coverage.HaveAnchor && coverage.Valid && !coverage.RollbackDetected,
		RollbackDetected: coverage.RollbackDetected,
	}
	if lastMismatch > 0 {
		at := time.Unix(int64(lastMismatch), 0).UTC()
		status.LastMismatchAt = &at
	}
	startSec := coverage.LowUtime + 1
	endSec := coverage.HighUtime
	if coverage.HaveAnchor && startSec < endSec {
		start := time.Unix(startSec, 0).UTC()
		end := time.Unix(endSec, 0).UTC()
		status.CoverageStart = &start
		status.CoverageEnd = &end
	}
	return status, nil
}

func recorderState(epoch epochData) (RecorderState, error) {
	cp := Checkpoint{Shards: map[ShardID]uint32{}}
	if raw := epoch.meta.Get(checkpointKey); raw != nil {
		var err error
		cp, err = decodeCheckpoint(raw)
		if err != nil {
			return RecorderState{}, err
		}
	}
	coverage, err := readCoverage(epoch)
	if err != nil {
		return RecorderState{}, err
	}
	if cp.HaveMC != coverage.HaveAnchor || (cp.HaveMC && cp.MCSeqno != coverage.HighMC) {
		return RecorderState{}, errInvalidStoreData
	}
	if cp.HaveMC && cp.MCIdentity.isZero() {
		return RecorderState{}, errInvalidStoreData
	}
	status, err := storeStatus(epoch)
	if err != nil {
		return RecorderState{}, err
	}
	return RecorderState{Checkpoint: cp, Coverage: coverage, Status: status}, nil
}

func validateCanonicalBatch(stats []Stat, mcSeqno uint32, mcUtime int64) error {
	foundMC := false
	seen := make(map[BlockID]Stat, len(stats))
	for _, stat := range stats {
		if stat.TxCount < 0 {
			return fmt.Errorf("negative transaction count %d", stat.TxCount)
		}
		if stat.Partial {
			return fmt.Errorf("partial block %+v cannot advance canonical coverage", stat.ID)
		}
		if old, ok := seen[stat.ID]; ok {
			if old != stat {
				return fmt.Errorf("conflicting duplicate block %+v", stat.ID)
			}
			continue
		}
		seen[stat.ID] = stat
		if stat.ID == (BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: mcSeqno}) && stat.IsMC && stat.GenUtime == mcUtime {
			foundMC = true
		}
	}
	if !foundMC {
		return fmt.Errorf("canonical batch is missing MC %d at %d", mcSeqno, mcUtime)
	}
	return nil
}

// frontierBounds returns conservative time watermarks for one fully recorded MC
// snapshot. Unfetched predecessors are no newer than the maximum low top, while
// future successors are no older than the minimum high top.
func frontierBounds(epoch epochData, cp Checkpoint, mcUtime int64) (low, high int64, err error) {
	low, high = mcUtime, mcUtime
	blocks := epoch.blocks
	for shard, seqno := range cp.Shards {
		// Shard seqno zero is the zerostate, not a block with a retrievable
		// header or transactions.
		if seqno == 0 {
			continue
		}
		id := BlockID{Workchain: shard.Workchain, Shard: shard.Shard, Seqno: seqno}
		raw := blocks.Get(encodeBlockID(id))
		if raw == nil {
			return 0, 0, fmt.Errorf("checkpoint shard top %+v is not recorded", id)
		}
		stat, err := decodeStat(id, raw)
		if err != nil {
			return 0, 0, err
		}
		if stat.GenUtime > low {
			low = stat.GenUtime
		}
		if stat.GenUtime < high {
			high = stat.GenUtime
		}
	}
	return low, high, nil
}

func insertStats(epoch epochData, stats []Stat) (uint64, error) {
	var inserted uint64
	seen := make(map[BlockID]bool, len(stats))
	blocks := epoch.blocks
	for _, stat := range stats {
		if seen[stat.ID] {
			continue
		}
		seen[stat.ID] = true
		if raw := blocks.Get(encodeBlockID(stat.ID)); raw != nil {
			persisted, err := decodeStat(stat.ID, raw)
			if err != nil {
				return 0, err
			}
			if persisted != stat {
				return 0, fmt.Errorf("stored block %+v conflicts with canonical batch", stat.ID)
			}
			continue
		}
		ok, err := insertStat(epoch, stat)
		if err != nil {
			return 0, err
		}
		if ok {
			inserted++
		}
	}
	return inserted, nil
}

func insertStat(epoch epochData, stat Stat) (bool, error) {
	if stat.TxCount < 0 {
		return false, fmt.Errorf("negative transaction count %d", stat.TxCount)
	}
	blocks := epoch.blocks
	key := encodeBlockID(stat.ID)
	if blocks.Get(key) != nil {
		return false, nil
	}
	if err := blocks.Put(key, encodeStat(stat)); err != nil {
		return false, err
	}

	seconds := epoch.seconds
	secondKey := encodeSecond(stat.GenUtime)
	agg, err := decodeAggregate(seconds.Get(secondKey))
	if err != nil {
		return false, err
	}
	txCount := uint64(stat.TxCount)
	if stat.IsMC {
		if math.MaxUint64-agg.MCTx < txCount || agg.MCBlocks == math.MaxUint64 {
			return false, fmt.Errorf("masterchain aggregate overflow at %d", stat.GenUtime)
		}
		agg.MCTx += txCount
		agg.MCBlocks++
	} else {
		if math.MaxUint64-agg.WCTx < txCount || agg.WCBlocks == math.MaxUint64 {
			return false, fmt.Errorf("workchain aggregate overflow at %d", stat.GenUtime)
		}
		agg.WCTx += txCount
		agg.WCBlocks++
	}
	if stat.Partial {
		if agg.PartialBlocks == math.MaxUint64 {
			return false, fmt.Errorf("partial aggregate overflow at %d", stat.GenUtime)
		}
		agg.PartialBlocks++
	}
	if err := seconds.Put(secondKey, encodeAggregate(agg)); err != nil {
		return false, err
	}

	meta := epoch.meta
	count, err := readUint64(meta, blockCountKey)
	if err != nil {
		return false, err
	}
	if count == math.MaxUint64 {
		return false, fmt.Errorf("block count overflow")
	}
	if err := putUint64(meta, blockCountKey, count+1); err != nil {
		return false, err
	}
	if stat.Partial {
		partials, err := readUint64(meta, partialCountKey)
		if err != nil {
			return false, err
		}
		if partials == math.MaxUint64 {
			return false, fmt.Errorf("partial count overflow")
		}
		if err := putUint64(meta, partialCountKey, partials+1); err != nil {
			return false, err
		}
	}
	return true, nil
}

func writeCheckpoint(epoch epochData, cp Checkpoint) error {
	return epoch.meta.Put(checkpointKey, encodeCheckpoint(cp))
}

type shardSample struct {
	MCSeqno uint32
	Count   uint32
}

func writeShardSample(epoch epochData, mcUtime int64, cp Checkpoint) error {
	if uint64(len(cp.Shards)) > math.MaxUint32 {
		return fmt.Errorf("shard count overflow")
	}
	bucket := epoch.shards
	if bucket == nil {
		var err error
		bucket, err = epoch.bucket.CreateBucket(shardsBucket)
		if err != nil {
			return err
		}
	}
	key := encodeSecond(mcUtime)
	sample := shardSample{MCSeqno: cp.MCSeqno, Count: uint32(len(cp.Shards))}
	if raw := bucket.Get(key); raw != nil {
		old, err := decodeShardSample(raw)
		if err != nil {
			return err
		}
		if old.MCSeqno > sample.MCSeqno {
			return nil
		}
		if old.MCSeqno == sample.MCSeqno && old.Count != sample.Count {
			return errInvalidStoreData
		}
	}
	return bucket.Put(key, encodeShardSample(sample))
}

func writeCoverage(epoch epochData, coverage CoverageState) error {
	return epoch.meta.Put(coverageKey, encodeCoverage(coverage))
}

func readCoverage(epoch epochData) (CoverageState, error) {
	raw := epoch.meta.Get(coverageKey)
	if raw == nil {
		return CoverageState{LowShards: map[ShardID]uint32{}}, nil
	}
	return decodeCoverage(raw)
}

func cloneShardIDs(in map[ShardID]uint32) map[ShardID]uint32 {
	out := make(map[ShardID]uint32, len(in))
	for key, seqno := range in {
		out[key] = seqno
	}
	return out
}

func encodeBlockID(id BlockID) []byte {
	raw := make([]byte, 16)
	binary.BigEndian.PutUint32(raw[0:4], uint32(id.Workchain))
	binary.BigEndian.PutUint64(raw[4:12], uint64(id.Shard))
	binary.BigEndian.PutUint32(raw[12:16], id.Seqno)
	return raw
}

func encodeStat(stat Stat) []byte {
	raw := make([]byte, 17)
	binary.BigEndian.PutUint64(raw[0:8], uint64(stat.GenUtime))
	binary.BigEndian.PutUint64(raw[8:16], uint64(stat.TxCount))
	if stat.Partial {
		raw[16] |= 1
	}
	if stat.IsMC {
		raw[16] |= 2
	}
	return raw
}

func decodeStat(id BlockID, raw []byte) (Stat, error) {
	if len(raw) != 17 {
		return Stat{}, errInvalidStoreData
	}
	txCount := binary.BigEndian.Uint64(raw[8:16])
	if txCount > uint64(math.MaxInt) {
		return Stat{}, errInvalidStoreData
	}
	return Stat{
		ID:       id,
		GenUtime: int64(binary.BigEndian.Uint64(raw[0:8])),
		TxCount:  int(txCount),
		Partial:  raw[16]&1 != 0,
		IsMC:     raw[16]&2 != 0,
	}, nil
}

func encodeSecond(second int64) []byte {
	raw := make([]byte, 8)
	binary.BigEndian.PutUint64(raw, uint64(second)^(uint64(1)<<63))
	return raw
}

func decodeSecond(raw []byte) (int64, error) {
	if len(raw) != 8 {
		return 0, errInvalidStoreData
	}
	return int64(binary.BigEndian.Uint64(raw) ^ (uint64(1) << 63)), nil
}

func encodeAggregate(agg secondAggregate) []byte {
	raw := make([]byte, 40)
	binary.BigEndian.PutUint64(raw[0:8], agg.MCTx)
	binary.BigEndian.PutUint64(raw[8:16], agg.WCTx)
	binary.BigEndian.PutUint64(raw[16:24], agg.MCBlocks)
	binary.BigEndian.PutUint64(raw[24:32], agg.WCBlocks)
	binary.BigEndian.PutUint64(raw[32:40], agg.PartialBlocks)
	return raw
}

func decodeAggregate(raw []byte) (secondAggregate, error) {
	if raw == nil {
		return secondAggregate{}, nil
	}
	if len(raw) != 40 {
		return secondAggregate{}, errInvalidStoreData
	}
	return secondAggregate{
		MCTx:          binary.BigEndian.Uint64(raw[0:8]),
		WCTx:          binary.BigEndian.Uint64(raw[8:16]),
		MCBlocks:      binary.BigEndian.Uint64(raw[16:24]),
		WCBlocks:      binary.BigEndian.Uint64(raw[24:32]),
		PartialBlocks: binary.BigEndian.Uint64(raw[32:40]),
	}, nil
}

func encodeShardSample(sample shardSample) []byte {
	raw := make([]byte, 8)
	binary.BigEndian.PutUint32(raw[0:4], sample.MCSeqno)
	binary.BigEndian.PutUint32(raw[4:8], sample.Count)
	return raw
}

func decodeShardSample(raw []byte) (shardSample, error) {
	if len(raw) != 8 {
		return shardSample{}, errInvalidStoreData
	}
	return shardSample{
		MCSeqno: binary.BigEndian.Uint32(raw[0:4]),
		Count:   binary.BigEndian.Uint32(raw[4:8]),
	}, nil
}

func encodeCheckpoint(cp Checkpoint) []byte {
	keys := make([]ShardID, 0, len(cp.Shards))
	for key := range cp.Shards {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].Workchain != keys[j].Workchain {
			return keys[i].Workchain < keys[j].Workchain
		}
		return keys[i].Shard < keys[j].Shard
	})
	const headerSize = 74
	raw := make([]byte, headerSize+len(keys)*16)
	if cp.HaveMC {
		raw[0] = 1
	}
	binary.BigEndian.PutUint32(raw[1:5], cp.MCSeqno)
	binary.BigEndian.PutUint32(raw[5:9], uint32(len(keys)))
	raw[9] = 2
	copy(raw[10:42], cp.MCIdentity.RootHash[:])
	copy(raw[42:74], cp.MCIdentity.FileHash[:])
	for i, key := range keys {
		off := headerSize + i*16
		binary.BigEndian.PutUint32(raw[off:off+4], uint32(key.Workchain))
		binary.BigEndian.PutUint64(raw[off+4:off+12], uint64(key.Shard))
		binary.BigEndian.PutUint32(raw[off+12:off+16], cp.Shards[key])
	}
	return raw
}

func decodeCheckpoint(raw []byte) (Checkpoint, error) {
	const headerSize = 74
	if len(raw) < headerSize || raw[9] != 2 {
		return Checkpoint{}, errInvalidStoreData
	}
	count := binary.BigEndian.Uint32(raw[5:9])
	if uint64(count) > uint64((len(raw)-headerSize)/16) || len(raw) != headerSize+int(count)*16 {
		return Checkpoint{}, errInvalidStoreData
	}
	cp := Checkpoint{
		HaveMC:  raw[0] != 0,
		MCSeqno: binary.BigEndian.Uint32(raw[1:5]),
		Shards:  make(map[ShardID]uint32, count),
	}
	copy(cp.MCIdentity.RootHash[:], raw[10:42])
	copy(cp.MCIdentity.FileHash[:], raw[42:74])
	for i := uint32(0); i < count; i++ {
		off := headerSize + int(i)*16
		key := ShardID{
			Workchain: int32(binary.BigEndian.Uint32(raw[off : off+4])),
			Shard:     int64(binary.BigEndian.Uint64(raw[off+4 : off+12])),
		}
		cp.Shards[key] = binary.BigEndian.Uint32(raw[off+12 : off+16])
	}
	return cp, nil
}

func encodeCoverage(coverage CoverageState) []byte {
	keys := make([]ShardID, 0, len(coverage.LowShards))
	for key := range coverage.LowShards {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].Workchain != keys[j].Workchain {
			return keys[i].Workchain < keys[j].Workchain
		}
		return keys[i].Shard < keys[j].Shard
	})
	raw := make([]byte, 30+len(keys)*16)
	raw[0] = 1
	if coverage.HaveAnchor {
		raw[1] |= 1
	}
	if coverage.Valid {
		raw[1] |= 2
	}
	if coverage.RollbackDetected {
		raw[1] |= 4
	}
	binary.BigEndian.PutUint32(raw[2:6], coverage.LowMC)
	binary.BigEndian.PutUint32(raw[6:10], coverage.HighMC)
	binary.BigEndian.PutUint64(raw[10:18], uint64(coverage.LowUtime))
	binary.BigEndian.PutUint64(raw[18:26], uint64(coverage.HighUtime))
	binary.BigEndian.PutUint32(raw[26:30], uint32(len(keys)))
	for i, key := range keys {
		off := 30 + i*16
		binary.BigEndian.PutUint32(raw[off:off+4], uint32(key.Workchain))
		binary.BigEndian.PutUint64(raw[off+4:off+12], uint64(key.Shard))
		binary.BigEndian.PutUint32(raw[off+12:off+16], coverage.LowShards[key])
	}
	return raw
}

func decodeCoverage(raw []byte) (CoverageState, error) {
	if len(raw) < 30 || raw[0] != 1 {
		return CoverageState{}, errInvalidStoreData
	}
	count := binary.BigEndian.Uint32(raw[26:30])
	if uint64(count) > uint64((len(raw)-30)/16) || len(raw) != 30+int(count)*16 {
		return CoverageState{}, errInvalidStoreData
	}
	coverage := CoverageState{
		HaveAnchor:       raw[1]&1 != 0,
		Valid:            raw[1]&2 != 0,
		RollbackDetected: raw[1]&4 != 0,
		LowMC:            binary.BigEndian.Uint32(raw[2:6]),
		HighMC:           binary.BigEndian.Uint32(raw[6:10]),
		LowUtime:         int64(binary.BigEndian.Uint64(raw[10:18])),
		HighUtime:        int64(binary.BigEndian.Uint64(raw[18:26])),
		LowShards:        make(map[ShardID]uint32, count),
	}
	for i := uint32(0); i < count; i++ {
		off := 30 + int(i)*16
		key := ShardID{
			Workchain: int32(binary.BigEndian.Uint32(raw[off : off+4])),
			Shard:     int64(binary.BigEndian.Uint64(raw[off+4 : off+12])),
		}
		coverage.LowShards[key] = binary.BigEndian.Uint32(raw[off+12 : off+16])
	}
	if coverage.HaveAnchor && coverage.LowMC > coverage.HighMC {
		return CoverageState{}, errInvalidStoreData
	}
	return coverage, nil
}

func readUint64(bucket *bolt.Bucket, key []byte) (uint64, error) {
	raw := bucket.Get(key)
	if raw == nil {
		return 0, nil
	}
	return decodeUint64(raw)
}

func readRequiredUint64(bucket *bolt.Bucket, key []byte) (uint64, error) {
	if bucket == nil {
		return 0, errInvalidStoreData
	}
	raw := bucket.Get(key)
	if raw == nil {
		return 0, errInvalidStoreData
	}
	return decodeUint64(raw)
}

func decodeUint64(raw []byte) (uint64, error) {
	if len(raw) != 8 {
		return 0, errInvalidStoreData
	}
	return binary.BigEndian.Uint64(raw), nil
}

func putUint64(bucket *bolt.Bucket, key []byte, value uint64) error {
	raw := make([]byte, 8)
	binary.BigEndian.PutUint64(raw, value)
	return bucket.Put(key, raw)
}

func encodeEpoch(epoch uint64) []byte {
	raw := make([]byte, 8)
	binary.BigEndian.PutUint64(raw, epoch)
	return raw
}

func decodeEpoch(raw []byte) (uint64, error) {
	epoch, err := decodeUint64(raw)
	if err != nil || epoch == 0 {
		return 0, errInvalidStoreData
	}
	return epoch, nil
}
