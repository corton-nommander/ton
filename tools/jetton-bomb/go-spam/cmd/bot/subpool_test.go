package main

import (
	"testing"
)

func testRuntime(total, sub int) *Runtime {
	r := &Runtime{poolTotal: total}
	r.subPoolSize.Store(int64(sub))
	return r
}

// The whole point of fixed slices over a random offset: every wallet must be reachable by exactly ONE
// window, so no part of the pool is spent faster than another. A random-offset scheme would make the
// middle reachable by ~size windows and the two ends by one, wearing the middle out first.
func TestPickSubPoolSlicesAreAligned(t *testing.T) {
	const total, size = 1000, 100
	r := testRuntime(total, size)

	seen := map[int]int{}
	for i := 0; i < 20000; i++ {
		start, n := r.pickSubPool()
		if n != size {
			t.Fatalf("size %d, want %d", n, size)
		}
		if start%size != 0 {
			t.Fatalf("start %d is not aligned to %d — windows overlap, coverage is uneven", start, size)
		}
		if start < 0 || start+n > total {
			t.Fatalf("window [%d,%d) escapes the pool of %d", start, start+n, total)
		}
		seen[start]++
	}
	if len(seen) != total/size {
		t.Fatalf("only %d of %d sub-pools ever picked", len(seen), total/size)
	}
	// Uniformity: with 20k draws over 10 slices,each slice should land near 2000. A 2x band is loose enough
	// to never flake and tight enough to catch a skewed picker.
	for start, n := range seen {
		if n < 1000 || n > 3000 {
			t.Errorf("sub-pool at %d drawn %d times — expected ~2000, picker is skewed", start, n)
		}
	}
}

// A pool that does not divide evenly must drop the tail rather than emit a short window: a short slice
// would get the same odds as a full one, so its wallets would be spent several times faster.
func TestPickSubPoolDropsRemainder(t *testing.T) {
	const total, size = 250, 100 // 2 full slices + 50 left over
	r := testRuntime(total, size)
	for i := 0; i < 5000; i++ {
		start, n := r.pickSubPool()
		if n != size {
			t.Fatalf("emitted a short window of %d", n)
		}
		if start != 0 && start != 100 {
			t.Fatalf("start %d — the 50-wallet tail must never be a window start", start)
		}
	}
}

// Small deployments (a 5k mainnet pool with the default 100k size) must run whole-pool, never error or
// emit a window past the end.
func TestPickSubPoolClampsToSmallPool(t *testing.T) {
	r := testRuntime(5000, DefaultSubPoolSize)
	if got := r.SubPoolSize(); got != 5000 {
		t.Fatalf("SubPoolSize %d, want it clamped to the deployed 5000", got)
	}
	for i := 0; i < 100; i++ {
		start, n := r.pickSubPool()
		if start != 0 || n != 5000 {
			t.Fatalf("window [%d,%d), want the whole 5000-wallet pool", start, start+n)
		}
	}
}

// An unset/zero size must fall back to the default rather than producing a zero-width window that would
// send nothing at all.
func TestSubPoolSizeFallsBackToDefault(t *testing.T) {
	r := testRuntime(DefaultPoolTotal, 0)
	if got := r.SubPoolSize(); got != DefaultSubPoolSize {
		t.Fatalf("SubPoolSize %d, want the %d default", got, DefaultSubPoolSize)
	}
}

// shortCount is what every pool size in chat goes through, so a formatting slip is user-visible on every
// run. The "2.5MM" case is not hypothetical: an over-eager suffix trim produced exactly that.
func TestShortCount(t *testing.T) {
	for _, c := range []struct {
		in   int
		want string
	}{
		{100000, "100K"}, {5000, "5K"}, {431000, "431K"}, {1000000, "1M"},
		{2000000, "2M"}, {2100000, "2.1M"}, {2400000, "2.4M"}, {2500000, "2.5M"},
		{123, "123"}, {2500123, "2 500 123"},
	} {
		if got := shortCount(c.in); got != c.want {
			t.Errorf("shortCount(%d) = %q, want %q", c.in, got, c.want)
		}
	}
}

// The plain-text range (audit + logs) must stay half-open and carry the total, so a run is identifiable
// afterwards from the log alone.
func TestPoolRangeText(t *testing.T) {
	if got, want := poolRangeText(2000000, 100000, 2500000), "[2M, 2.1M) of 2.5M"; got != want {
		t.Errorf("got %q, want %q", got, want)
	}
	if got, want := poolRangeText(0, 5000, 5000), "(whole pool of 5K)"; got != want {
		t.Errorf("whole-pool case: got %q, want %q", got, want)
	}
}
