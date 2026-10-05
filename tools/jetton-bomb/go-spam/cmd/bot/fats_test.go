package main

import "testing"

// The fat-target window is sliced out of a derived address list, so an off-by-one in pickFatWindow is a
// panic on a live run, not a wrong number. Lock the invariant the slice expression depends on:
// 0 <= start and start+count <= total, for every count from 1 to past the tier size.
func TestPickFatWindowStaysInBounds(t *testing.T) {
	const total = 25000
	r := &Runtime{}
	for _, n := range []int{0, 1, 2, 3, 7, 999, 1000, 12500, 24999, total, total + 1, 1_000_000} {
		r.fatCount.Store(int64(n))
		for i := 0; i < 200; i++ {
			start, count := r.pickFatWindow(total)
			if count != r.FatCount(total) {
				t.Fatalf("fat_count=%d: window count %d, want %d", n, count, r.FatCount(total))
			}
			if start < 0 || start+count > total {
				t.Fatalf("fat_count=%d: window [%d, %d) out of tier [0, %d)", n, start, start+count, total)
			}
			if start%count != 0 {
				t.Fatalf("fat_count=%d: window start %d is not slice-aligned", n, start)
			}
		}
	}
}

// 0 means "unset" and anything past the tier is clamped, so a /fat_count left over from a bigger tier can
// never out-run a smaller one even before /fat_size resets it.
func TestFatCountDefaultsToWholeTier(t *testing.T) {
	r := &Runtime{}
	if got := r.FatCount(40000); got != 40000 {
		t.Fatalf("unset fat_count: got %d, want the whole tier 40000", got)
	}
	r.fatCount.Store(50000)
	if got := r.FatCount(25000); got != 25000 {
		t.Fatalf("fat_count past the tier: got %d, want it clamped to 25000", got)
	}
	r.fatCount.Store(1000)
	if got := r.FatCount(25000); got != 1000 {
		t.Fatalf("fat_count within the tier: got %d, want 1000", got)
	}
}
