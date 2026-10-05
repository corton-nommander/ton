// Package drain tracks which pool wallets have run below the send threshold during a spam run, so callers
// can skip them with no RPC, pick live recipients, and auto-stop once too much of the pool is drained.
// Used by cmd/bot (/check_balance) and cmd/spam (--check-balance). Balance only falls during a run (no
// incoming funds), so "drained" is monotonic — once a wallet drops below threshold it stays there, which
// is what makes the map a safe cache.
package drain

import (
	mathrand "math/rand"
	"sync"
)

// StopPercent is the share of the pool that must be marked drained before a run auto-stops.
const StopPercent = 75

// Tracker records which pool wallets (by subwallet id) have been observed below the send threshold.
// A known-drained wallet is skipped with zero RPC, recipients are chosen from the live set, and onStop
// fires exactly once when StopPercent of the pool is drained. Safe for concurrent use.
type Tracker struct {
	mu      sync.RWMutex
	drained map[int]struct{}
	count   int  // pool id space [0,count)
	stopAt  int  // len(drained) >= stopAt => onStop
	stopped bool // onStop already fired
	onStop  func()
}

// New builds a tracker over ids [0,count); onStop is invoked once when the drained set first reaches
// StopPercent of count (may be nil).
func New(count int, onStop func()) *Tracker {
	stopAt := count * StopPercent / 100
	if stopAt < 1 {
		stopAt = 1
	}
	return &Tracker{drained: make(map[int]struct{}, count/4+1), count: count, stopAt: stopAt, onStop: onStop}
}

// IsDrained reports whether id is known-drained (skip it with no RPC).
func (d *Tracker) IsDrained(id int) bool {
	d.mu.RLock()
	_, ok := d.drained[id]
	d.mu.RUnlock()
	return ok
}

// Mark records id as drained. The first crossing of stopAt fires onStop exactly once (outside the lock).
func (d *Tracker) Mark(id int) {
	d.mu.Lock()
	if _, ok := d.drained[id]; ok {
		d.mu.Unlock()
		return
	}
	d.drained[id] = struct{}{}
	fire := !d.stopped && len(d.drained) >= d.stopAt
	if fire {
		d.stopped = true
	}
	d.mu.Unlock()
	if fire && d.onStop != nil {
		d.onStop()
	}
}

// Count is the number of drained wallets.
func (d *Tracker) Count() int {
	d.mu.RLock()
	defer d.mu.RUnlock()
	return len(d.drained)
}

// RandomLive returns a random not-known-drained id via bounded rejection sampling. Because the run
// auto-stops at StopPercent, at most that fraction is drained, so a handful of tries suffices; the
// fallback (any id) only bites in the brief window right at the stop threshold.
func (d *Tracker) RandomLive() int {
	for i := 0; i < 8; i++ {
		id := mathrand.Intn(d.count)
		if !d.IsDrained(id) {
			return id
		}
	}
	return mathrand.Intn(d.count)
}
