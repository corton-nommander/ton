package tpsmeter

import (
	"fmt"
	"sort"
	"time"

	bolt "go.etcd.io/bbolt"
)

// A dashboard range selector goes as wide as the history the store holds, and
// chbackfill loads a month of it.
const maxQueryRangeSeconds int64 = 32 * 24 * 60 * 60

// BlockRates is a block-production rate triple, in blocks per second. WC sums
// every shard, so it doubles when a shard splits even though each shard keeps
// its cadence; WCPerShard is the per-shard cadence — WC divided by the shard
// count of each second, so it stays flat across a split. Shards is the mean
// shard count over the same interval. Both are nil when no shard sample covers
// the interval.
type BlockRates struct {
	Total      float64  `json:"total"`
	MC         float64  `json:"mc"`
	WC         float64  `json:"wc"`
	WCPerShard *float64 `json:"wc_per_shard"`
	Shards     *float64 `json:"shards"`
}

// SummaryResult is the exact aggregate for a run interval [Start, End).
type SummaryResult struct {
	Start               time.Time   `json:"start"`
	End                 time.Time   `json:"end"`
	WindowSize          int         `json:"window_size"`
	RunAverage          float64     `json:"run_avg"`
	RunBlockRate        BlockRates  `json:"run_block_rate"`
	LastWindowBlockRate *BlockRates `json:"last_window_block_rate"`
	LastWindow          *float64    `json:"last_window"`
	MaxWindow           *float64    `json:"max_window"`
	MaxWindowStart      *time.Time  `json:"max_window_start"`
	MaxWindowEnd        *time.Time  `json:"max_window_end"`
	Complete            bool        `json:"complete"`
	PartialBlocks       uint64      `json:"partial_blocks"`
	Epochs              []uint64    `json:"epochs,omitempty"`
	IncludesActiveEpoch bool        `json:"includes_active_epoch"`
	EpochOverlap        bool        `json:"epoch_overlap,omitempty"`
}

// SeriesPoint is one trailing TPS value for [Timestamp-WindowSize, Timestamp).
// The *BlockRate fields are the block rate over the same window, in blocks per
// second; WCBlockRate sums every shard, so per-shard cadence is WCBlockRate/Shards.
type SeriesPoint struct {
	Timestamp   time.Time `json:"timestamp"`
	Total       float64   `json:"total"`
	MC          float64   `json:"mc"`
	WC          float64   `json:"wc"`
	BlockRate   float64   `json:"block_rate"`
	MCBlockRate float64   `json:"mc_block_rate"`
	WCBlockRate float64   `json:"wc_block_rate"`
	Shards      *int      `json:"shards"`
}

// SeriesResult is a rolling TPS series over a run interval.
type SeriesResult struct {
	Start               time.Time     `json:"start"`
	End                 time.Time     `json:"end"`
	WindowSize          int           `json:"window_size"`
	Step                int           `json:"step"`
	Complete            bool          `json:"complete"`
	PartialBlocks       uint64        `json:"partial_blocks"`
	Epochs              []uint64      `json:"epochs,omitempty"`
	IncludesActiveEpoch bool          `json:"includes_active_epoch"`
	EpochOverlap        bool          `json:"epoch_overlap,omitempty"`
	Points              []SeriesPoint `json:"points"`
}

// Summary calculates run, trailing-window, and exhaustive integer-second max
// window TPS for [start, end) across epochs. The newest epoch owns overlaps.
func (s *Store) Summary(start, end time.Time, windowSec int) (SummaryResult, error) {
	startSec, endSec, err := validateRange(start, end, windowSec)
	if err != nil {
		return SummaryResult{}, err
	}
	data, err := s.loadQueryRange(startSec, endSec)
	if err != nil {
		return SummaryResult{}, err
	}

	duration := endSec - startSec
	runAgg := data.aggregate(startSec, endSec)
	runTotal := runAgg.MCTx + runAgg.WCTx
	result := SummaryResult{
		Start:               time.Unix(startSec, 0).UTC(),
		End:                 time.Unix(endSec, 0).UTC(),
		WindowSize:          windowSec,
		RunAverage:          float64(runTotal) / float64(duration),
		RunBlockRate:        data.blockRatesOver(startSec, endSec),
		Complete:            data.complete,
		PartialBlocks:       data.partialBlocks,
		Epochs:              data.epochs,
		IncludesActiveEpoch: data.includesActiveEpoch,
		EpochOverlap:        data.epochOverlap,
	}
	if duration < int64(windowSec) {
		return result, nil
	}

	window := int64(windowSec)
	lastAgg := data.aggregate(endSec-window, endSec)
	last := float64(lastAgg.MCTx+lastAgg.WCTx) / float64(window)
	result.LastWindow = &last
	lastBlockRate := data.blockRatesOver(endSec-window, endSec)
	result.LastWindowBlockRate = &lastBlockRate

	windowTotal := data.total(startSec, startSec+window)
	maxTotal := windowTotal
	maxStartSec := startSec
	for candidate := startSec + 1; candidate <= endSec-window; candidate++ {
		windowTotal -= data.total(candidate-1, candidate)
		windowTotal += data.total(candidate+window-1, candidate+window)
		if windowTotal > maxTotal {
			maxTotal = windowTotal
			maxStartSec = candidate
		}
	}
	maxTPS := float64(maxTotal) / float64(window)
	maxStart := time.Unix(maxStartSec, 0).UTC()
	maxEnd := time.Unix(maxStartSec+window, 0).UTC()
	result.MaxWindow = &maxTPS
	result.MaxWindowStart = &maxStart
	result.MaxWindowEnd = &maxEnd
	return result, nil
}

// Series returns trailing windows across epochs whose timestamps run from
// start+W through end. A zero step selects a cadence of about 1000 points.
func (s *Store) Series(start, end time.Time, windowSec, step int) (SeriesResult, error) {
	startSec, endSec, err := validateRange(start, end, windowSec)
	if err != nil {
		return SeriesResult{}, err
	}
	if step < 0 {
		return SeriesResult{}, fmt.Errorf("step must be positive")
	}
	duration := endSec - startSec
	if step == 0 {
		step = autoStep(duration, int64(windowSec))
	}
	data, err := s.loadQueryRange(startSec, endSec)
	if err != nil {
		return SeriesResult{}, err
	}
	result := SeriesResult{
		Start:               time.Unix(startSec, 0).UTC(),
		End:                 time.Unix(endSec, 0).UTC(),
		WindowSize:          windowSec,
		Step:                step,
		Complete:            data.complete,
		PartialBlocks:       data.partialBlocks,
		Epochs:              data.epochs,
		IncludesActiveEpoch: data.includesActiveEpoch,
		EpochOverlap:        data.epochOverlap,
		Points:              []SeriesPoint{},
	}
	window := int64(windowSec)
	for timestamp := startSec + window; timestamp <= endSec; timestamp += int64(step) {
		agg := data.aggregate(timestamp-window, timestamp)
		mc := float64(agg.MCTx) / float64(window)
		wc := float64(agg.WCTx) / float64(window)
		mcBlocks := float64(agg.MCBlocks) / float64(window)
		wcBlocks := float64(agg.WCBlocks) / float64(window)
		result.Points = append(result.Points, SeriesPoint{
			Timestamp:   time.Unix(timestamp, 0).UTC(),
			Total:       mc + wc,
			MC:          mc,
			WC:          wc,
			BlockRate:   mcBlocks + wcBlocks,
			MCBlockRate: mcBlocks,
			WCBlockRate: wcBlocks,
			Shards:      data.shardsAt(timestamp),
		})
	}
	return result, nil
}

type queryRange struct {
	seconds             map[int64]secondAggregate
	owners              map[int64]uint64
	shards              map[uint64][]shardCountSample
	partialBlocks       uint64
	epochs              []uint64
	includesActiveEpoch bool
	epochOverlap        bool
	complete            bool
}

type epochQueryRange struct {
	id      uint64
	status  StoreStatus
	seconds map[int64]secondAggregate
	shards  []shardCountSample
}

type shardCountSample struct {
	timestamp int64
	count     int
}

func (s *Store) loadQueryRange(start, end int64) (queryRange, error) {
	data := queryRange{
		seconds:  map[int64]secondAggregate{},
		owners:   map[int64]uint64{},
		shards:   map[uint64][]shardCountSample{},
		complete: true,
	}
	var activeEpoch uint64
	var epochRanges []epochQueryRange
	err := s.db.View(func(tx *bolt.Tx) error {
		active, err := activeEpochData(tx)
		if err != nil {
			return err
		}
		activeEpoch = active.id
		epochs := tx.Bucket(epochsBucket)
		cursor := epochs.Cursor()
		for key, _ := cursor.First(); key != nil; key, _ = cursor.Next() {
			epochID, err := decodeEpoch(key)
			if err != nil {
				return err
			}
			epoch, err := epochDataFromBucket(epochID, epochs.Bucket(key))
			if err != nil {
				return err
			}
			status, err := storeStatus(epoch)
			if err != nil {
				return err
			}
			rangeData := epochQueryRange{id: epochID, status: status, seconds: map[int64]secondAggregate{}}
			seconds := epoch.seconds.Cursor()
			for secondKey, value := seconds.Seek(encodeSecond(start)); secondKey != nil; secondKey, value = seconds.Next() {
				second, err := decodeSecond(secondKey)
				if err != nil {
					return err
				}
				if second >= end {
					break
				}
				agg, err := decodeAggregate(value)
				if err != nil {
					return err
				}
				rangeData.seconds[second] = agg
			}
			rangeData.shards, err = loadShardSamples(epoch.shards, start, end)
			if err != nil {
				return err
			}
			epochRanges = append(epochRanges, rangeData)
		}
		return nil
	})
	if err != nil {
		return queryRange{}, fmt.Errorf("query store range: %w", err)
	}
	encountered := make(map[uint64]bool, len(epochRanges))
	selectedComplete := make(map[int64]bool)
	selectSecond := func(epoch epochQueryRange, second int64, agg secondAggregate, covered bool) {
		if _, exists := data.owners[second]; exists {
			data.epochOverlap = true
		}
		data.owners[second] = epoch.id
		data.seconds[second] = agg
		selectedComplete[second] = covered && epoch.status.CoverageValid && agg.PartialBlocks == 0
		encountered[epoch.id] = true
		if epoch.id == activeEpoch {
			data.includesActiveEpoch = true
		}
	}
	for _, epoch := range epochRanges {
		data.shards[epoch.id] = epoch.shards
		coverageStart, coverageEnd, covered := clippedStatusRange(epoch.status, start, end)
		if covered {
			for second := coverageStart; second < coverageEnd; second++ {
				selectSecond(epoch, second, epoch.seconds[second], true)
			}
		}
		for second, agg := range epoch.seconds {
			if statusContainsSecond(epoch.status, second) {
				continue
			}
			selectSecond(epoch, second, agg, false)
		}
	}
	for second := start; second < end; second++ {
		if _, ok := data.owners[second]; !ok || !selectedComplete[second] {
			data.complete = false
		}
		data.partialBlocks += data.seconds[second].PartialBlocks
	}
	if data.epochOverlap {
		data.complete = false
	}
	for _, epoch := range epochRanges {
		if encountered[epoch.id] {
			data.epochs = append(data.epochs, epoch.id)
		}
	}
	return data, nil
}

func loadShardSamples(bucket *bolt.Bucket, start, end int64) ([]shardCountSample, error) {
	if bucket == nil {
		return nil, nil
	}
	cursor := bucket.Cursor()
	key, value := cursor.Seek(encodeSecond(start))
	samples := []shardCountSample{}
	if key == nil {
		key, value = cursor.Last()
		if key == nil {
			return samples, nil
		}
		return appendShardSample(samples, key, value)
	}
	second, err := decodeSecond(key)
	if err != nil {
		return nil, err
	}
	if second > start {
		previousKey, previousValue := cursor.Prev()
		if previousKey != nil {
			samples, err = appendShardSample(samples, previousKey, previousValue)
			if err != nil {
				return nil, err
			}
		}
		key, value = cursor.Seek(encodeSecond(start))
	}
	for ; key != nil; key, value = cursor.Next() {
		second, err := decodeSecond(key)
		if err != nil {
			return nil, err
		}
		if second > end {
			break
		}
		samples, err = appendShardSample(samples, key, value)
		if err != nil {
			return nil, err
		}
	}
	return samples, nil
}

func appendShardSample(samples []shardCountSample, key, value []byte) ([]shardCountSample, error) {
	timestamp, err := decodeSecond(key)
	if err != nil {
		return nil, err
	}
	sample, err := decodeShardSample(value)
	if err != nil {
		return nil, err
	}
	return append(samples, shardCountSample{timestamp: timestamp, count: int(sample.Count)}), nil
}

// shardsIn is the shard count in force during second, from the newest sample at
// or before it. shardsAt answers for a window END (its owner is second-1), which
// is one second off when asking about the second itself.
func (q queryRange) shardsIn(second int64) *int {
	epoch, ok := q.owners[second]
	if !ok {
		return nil
	}
	return newestShardSample(q.shards[epoch], second)
}

func (q queryRange) shardsAt(timestamp int64) *int {
	epoch, ok := q.owners[timestamp-1]
	if !ok {
		return nil
	}
	return newestShardSample(q.shards[epoch], timestamp)
}

func newestShardSample(samples []shardCountSample, timestamp int64) *int {
	index := sort.Search(len(samples), func(i int) bool {
		return samples[i].timestamp > timestamp
	})
	if index == 0 {
		return nil
	}
	count := samples[index-1].count
	return &count
}

func statusContainsSecond(status StoreStatus, second int64) bool {
	return status.CoverageStart != nil && status.CoverageEnd != nil &&
		status.CoverageStart.Unix() <= second && second < status.CoverageEnd.Unix()
}

func clippedStatusRange(status StoreStatus, start, end int64) (int64, int64, bool) {
	if status.CoverageStart == nil || status.CoverageEnd == nil {
		return 0, 0, false
	}
	coverageStart, coverageEnd := status.CoverageStart.Unix(), status.CoverageEnd.Unix()
	if coverageStart < start {
		coverageStart = start
	}
	if coverageEnd > end {
		coverageEnd = end
	}
	return coverageStart, coverageEnd, coverageStart < coverageEnd
}

func (q queryRange) aggregate(start, end int64) secondAggregate {
	var total secondAggregate
	for second := start; second < end; second++ {
		agg := q.seconds[second]
		total.MCTx += agg.MCTx
		total.WCTx += agg.WCTx
		total.MCBlocks += agg.MCBlocks
		total.WCBlocks += agg.WCBlocks
		total.PartialBlocks += agg.PartialBlocks
	}
	return total
}

func blockRates(agg secondAggregate, seconds int64) BlockRates {
	if seconds <= 0 {
		return BlockRates{}
	}
	mc := float64(agg.MCBlocks) / float64(seconds)
	wc := float64(agg.WCBlocks) / float64(seconds)
	return BlockRates{Total: mc + wc, MC: mc, WC: wc}
}

// blockRatesOver is blockRates plus the per-shard WC cadence, which needs the
// per-second shard count (a split mid-interval changes the divisor).
func (q queryRange) blockRatesOver(start, end int64) BlockRates {
	rates := blockRates(q.aggregate(start, end), end-start)
	var perShard, shardSum float64
	var known int64
	for second := start; second < end; second++ {
		count := q.shardsIn(second)
		if count == nil || *count <= 0 {
			continue
		}
		perShard += float64(q.seconds[second].WCBlocks) / float64(*count)
		shardSum += float64(*count)
		known++
	}
	if known == 0 {
		return rates
	}
	rate := perShard / float64(known)
	shards := shardSum / float64(known)
	rates.WCPerShard = &rate
	rates.Shards = &shards
	return rates
}

func (q queryRange) total(start, end int64) uint64 {
	agg := q.aggregate(start, end)
	return agg.MCTx + agg.WCTx
}

func validateRange(start, end time.Time, windowSec int) (int64, int64, error) {
	if windowSec <= 0 {
		return 0, 0, fmt.Errorf("window_size must be positive")
	}
	if start.Nanosecond() != 0 || end.Nanosecond() != 0 {
		return 0, 0, fmt.Errorf("start and end must align to whole seconds")
	}
	startSec, endSec := start.Unix(), end.Unix()
	if endSec <= startSec {
		return 0, 0, fmt.Errorf("end must be after start")
	}
	if endSec-startSec > maxQueryRangeSeconds {
		return 0, 0, fmt.Errorf("range must not exceed 32 days")
	}
	return startSec, endSec, nil
}

func autoStep(duration, window int64) int {
	span := duration - window
	if span <= 0 {
		return 1
	}
	step := (span + 998) / 999
	if step < 1 {
		step = 1
	}
	return int(step)
}
