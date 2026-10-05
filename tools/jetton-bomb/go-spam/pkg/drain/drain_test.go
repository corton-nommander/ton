package drain

import (
	"sync"
	"sync/atomic"
	"testing"
)

func TestStopFiresOnceAt75(t *testing.T) {
	var fired atomic.Int64
	d := New(100, func() { fired.Add(1) })
	if d.stopAt != 75 {
		t.Fatalf("stopAt=%d want 75", d.stopAt)
	}
	for i := 0; i < 74; i++ { // below threshold, no stop
		d.Mark(i)
	}
	if fired.Load() != 0 {
		t.Fatalf("onStop fired at %d drained, want no fire before 75", d.Count())
	}
	d.Mark(74) // the 75th crossing fires exactly once
	if fired.Load() != 1 {
		t.Fatalf("onStop fired %d times crossing 75%%, want 1", fired.Load())
	}
	for i := 74; i < 100; i++ { // marking more (incl. re-marking) never fires again
		d.Mark(i)
	}
	if fired.Load() != 1 {
		t.Fatalf("onStop fired %d times total, want exactly 1", fired.Load())
	}
	if d.Count() != 100 {
		t.Fatalf("Count=%d want 100", d.Count())
	}
}

func TestMarkIsIdempotentAndConcurrent(t *testing.T) {
	var fired atomic.Int64
	d := New(1000, func() { fired.Add(1) })
	var wg sync.WaitGroup
	for g := 0; g < 8; g++ { // overlapping ids; the set must dedupe and onStop still fire once
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < 800; i++ {
				d.Mark(i)
			}
		}()
	}
	wg.Wait()
	if d.Count() != 800 {
		t.Fatalf("Count=%d want 800 (deduped)", d.Count())
	}
	if fired.Load() != 1 {
		t.Fatalf("onStop fired %d times, want exactly 1", fired.Load())
	}
}

func TestRandomLiveAvoidsDrained(t *testing.T) {
	d := New(10, nil)
	for i := 0; i < 10; i++ { // drain every id except 7
		if i != 7 {
			d.Mark(i)
		}
	}
	sawLive := false
	for i := 0; i < 200; i++ {
		if d.RandomLive() == 7 {
			sawLive = true
			break
		}
	}
	if !sawLive {
		t.Fatal("RandomLive never returned the only live id (7)")
	}
}
