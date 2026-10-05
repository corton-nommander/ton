package main

import (
	"strings"
	"testing"
	"time"
)

// ---------------------------------------------------------------------------
// progress.json parsing.
// ---------------------------------------------------------------------------

const validProgress = `{
  "schema": 1,
  "phase": "phase3-merge",
  "phase_name": "merge",
  "phase_index": 3,
  "phase_count": 4,
  "done": 127000000000,
  "total": 148000000000,
  "unit": "bytes",
  "phase_pct": 0.858,
  "global_pct": 0.712,
  "rate": 410000000.0,
  "eta_sec": 251,
  "elapsed_sec": 291,
  "accounts": 45010001,
  "accounts_total": 45000000,
  "seq": 97,
  "ts_unix": 1786140811,
  "done_flag": false
}`

func TestParseProgressValid(t *testing.T) {
	p, err := ParseProgress(validProgress)
	if err != nil {
		t.Fatalf("valid parse: %v", err)
	}
	if p.PhaseName != "merge" || p.PhaseIndex != 3 {
		t.Errorf("phase: %q/%d", p.PhaseName, p.PhaseIndex)
	}
	if p.GlobalPct != 0.712 || p.EtaSec != 251 || p.Seq != 97 {
		t.Errorf("fields: pct=%v eta=%d seq=%d", p.GlobalPct, p.EtaSec, p.Seq)
	}
	if p.AccountsTotal != 45000000 || p.Done != 127000000000 {
		t.Errorf("big ints: acc=%d done=%d", p.AccountsTotal, p.Done)
	}
}

func TestParseProgressPartialAndMissing(t *testing.T) {
	// Truncated / malformed JSON must error, not panic.
	if _, err := ParseProgress(`{"schema":1,"phase_name":"merge",`); err == nil {
		t.Error("truncated JSON should error")
	}
	// A snapshot that decodes but omits optional fields is fine (partial content).
	p, err := ParseProgress(`{"schema":1,"phase_name":"build","global_pct":0.2}`)
	if err != nil {
		t.Fatalf("partial-but-valid: %v", err)
	}
	if p.PhaseName != "build" || p.EtaSec != 0 || p.Seed != "" {
		t.Errorf("partial defaults: %+v", p)
	}
	// Missing/empty content errors.
	if _, err := ParseProgress(""); err == nil {
		t.Error("empty should error")
	}
	if _, err := ParseProgress("   \n "); err == nil {
		t.Error("whitespace should error")
	}
}

// ---------------------------------------------------------------------------
// Hosts file parsing.
// ---------------------------------------------------------------------------

func TestParseHostsFileCounted(t *testing.T) {
	got := ParseHostsFile("3\ndevnet-02.toncenter.com\ndevnet-03.toncenter.com\ndevnet-04.toncenter.com\n")
	want := []string{"devnet-02.toncenter.com", "devnet-03.toncenter.com", "devnet-04.toncenter.com"}
	if !equalStrings(got, want) {
		t.Errorf("counted: got %v want %v", got, want)
	}
}

func TestParseHostsFilePlainAndComments(t *testing.T) {
	got := ParseHostsFile("# comment\nhost-a\r\nhost-b\n\nhost-a\nhost-c\n")
	want := []string{"host-a", "host-b", "host-c"} // deduped, CR stripped, blanks+comments dropped
	if !equalStrings(got, want) {
		t.Errorf("plain: got %v want %v", got, want)
	}
}

// ---------------------------------------------------------------------------
// Tracker status classification.
// ---------------------------------------------------------------------------

func TestTrackerStatuses(t *testing.T) {
	now := time.Unix(1_000_000, 0)
	tr := NewTracker()

	tr.Observe(Sample{Host: "run", RawProgress: validProgress}, now)                // running
	tr.Observe(Sample{Host: "done", RawRC: "0"}, now)                               // done (rc authoritative)
	tr.Observe(Sample{Host: "fail", RawProgress: validProgress, RawRC: "137"}, now) // failed (rc wins over progress)
	tr.Observe(Sample{Host: "start"}, now)                                          // reachable, no files -> starting
	tr.Observe(Sample{Host: "gone", SSHFailed: true}, now)                          // unreachable, never had progress

	st := statesByHost(tr)
	if st["run"].Status != StatusRunning {
		t.Errorf("run: %v", st["run"].Status)
	}
	if st["done"].Status != StatusDone || st["done"].RC != 0 {
		t.Errorf("done: %v rc=%d", st["done"].Status, st["done"].RC)
	}
	if st["fail"].Status != StatusFailed || st["fail"].RC != 137 {
		t.Errorf("fail: %v rc=%d", st["fail"].Status, st["fail"].RC)
	}
	if st["start"].Status != StatusStarting {
		t.Errorf("start: %v", st["start"].Status)
	}
	if st["gone"].Status != StatusUnreachable {
		t.Errorf("gone: %v", st["gone"].Status)
	}
}

func TestTrackerUnreachableKeepsLastProgress(t *testing.T) {
	t0 := time.Unix(2_000_000, 0)
	tr := NewTracker()
	tr.Observe(Sample{Host: "n", RawProgress: validProgress}, t0)
	// Next tick ssh fails: keep last-known running state, don't advance.
	tr.Observe(Sample{Host: "n", SSHFailed: true}, t0.Add(3*time.Second))
	st := statesByHost(tr)["n"]
	if st.Status != StatusRunning || !st.HasProg {
		t.Fatalf("expected retained running, got %v hasProg=%v", st.Status, st.HasProg)
	}
	if !st.TickUnreachable {
		t.Error("expected TickUnreachable set")
	}
	if !st.LastAdvance.Equal(t0) {
		t.Errorf("LastAdvance should stay at t0, got %v", st.LastAdvance)
	}
}

// ---------------------------------------------------------------------------
// Aggregate: slowest selection, stall, done/failed.
// ---------------------------------------------------------------------------

func mkProg(phase string, pct float64, eta int64, seq int64) Progress {
	return Progress{PhaseName: phase, GlobalPct: pct, EtaSec: eta, Seq: seq, AccountsTotal: 45000000, ElapsedSec: 300}
}

func TestAggregateSlowest(t *testing.T) {
	now := time.Unix(5_000_000, 0)
	states := []NodeState{
		{Host: "fast", Status: StatusRunning, HasProg: true, Prog: mkProg("ingest", 0.90, 30, 5), LastAdvance: now, Now: now},
		{Host: "slow", Status: StatusRunning, HasProg: true, Prog: mkProg("merge", 0.40, 250, 5), LastAdvance: now, Now: now},
		{Host: "mid", Status: StatusRunning, HasProg: true, Prog: mkProg("build", 0.60, 120, 5), LastAdvance: now, Now: now},
		{Host: "fin", Status: StatusDone},
	}
	vm := Aggregate(states, Options{Interval: 3 * time.Second})

	if vm.SlowestHost != "slow" {
		t.Errorf("slowest host = %q, want slow", vm.SlowestHost)
	}
	if vm.SlowestPhase != "merge" {
		t.Errorf("slowest phase = %q, want merge", vm.SlowestPhase)
	}
	if vm.AggPct != 0.40 {
		t.Errorf("agg pct = %v, want 0.40", vm.AggPct)
	}
	if vm.DoneCount != 1 || vm.RunningCount != 3 || vm.AllDone {
		t.Errorf("counts: done=%d running=%d allDone=%v", vm.DoneCount, vm.RunningCount, vm.AllDone)
	}
	// The slowest node line is flagged.
	for _, n := range vm.Nodes {
		if n.Host == "slow" && !n.IsSlowest {
			t.Error("slow node not flagged IsSlowest")
		}
		if n.Host == "fast" && n.IsSlowest {
			t.Error("fast node wrongly flagged IsSlowest")
		}
	}
}

func TestAggregateStartingIsSlowest(t *testing.T) {
	now := time.Unix(6_000_000, 0)
	states := []NodeState{
		{Host: "running", Status: StatusRunning, HasProg: true, Prog: mkProg("build", 0.5, 100, 3), LastAdvance: now, Now: now},
		{Host: "queued", Status: StatusStarting, Now: now},
	}
	vm := Aggregate(states, Options{Interval: 3 * time.Second})
	if vm.SlowestHost != "queued" || vm.AggPct != 0 {
		t.Errorf("queued node should be slowest at 0%%: host=%q pct=%v", vm.SlowestHost, vm.AggPct)
	}
	if vm.StartingCount != 1 {
		t.Errorf("starting count = %d", vm.StartingCount)
	}
}

func TestAggregateStallDetection(t *testing.T) {
	base := time.Unix(7_000_000, 0)
	interval := 3 * time.Second
	// A node whose seq last advanced 10s ago (> 2×interval=6s) is stalled.
	stalledNode := NodeState{
		Host: "stuck", Status: StatusRunning, HasProg: true,
		Prog:        mkProg("merge", 0.33, 400, 12),
		LastAdvance: base,
		Now:         base.Add(10 * time.Second),
	}
	// A node that advanced 2s ago is fine.
	freshNode := NodeState{
		Host: "ok", Status: StatusRunning, HasProg: true,
		Prog:        mkProg("merge", 0.50, 200, 20),
		LastAdvance: base.Add(8 * time.Second),
		Now:         base.Add(10 * time.Second),
	}
	vm := Aggregate([]NodeState{stalledNode, freshNode}, Options{Interval: interval})

	byHost := map[string]NodeLine{}
	for _, n := range vm.Nodes {
		byHost[n.Host] = n
	}
	if !byHost["stuck"].Stalled {
		t.Error("stuck node should be stalled")
	}
	if byHost["stuck"].StalledFor != 10 {
		t.Errorf("stalledFor = %d, want 10", byHost["stuck"].StalledFor)
	}
	if byHost["ok"].Stalled {
		t.Error("fresh node should not be stalled")
	}
}

func TestAggregateAllDoneAndFailed(t *testing.T) {
	allDone := Aggregate([]NodeState{{Status: StatusDone}, {Status: StatusDone}}, Options{})
	if !allDone.AllDone || allDone.AggPct != 1.0 {
		t.Errorf("all done: allDone=%v pct=%v", allDone.AllDone, allDone.AggPct)
	}
	withFail := Aggregate([]NodeState{{Status: StatusDone}, {Host: "x", Status: StatusFailed, RC: 1}}, Options{})
	if !withFail.AnyFailed || withFail.FailedCount != 1 {
		t.Errorf("failed: any=%v count=%d", withFail.AnyFailed, withFail.FailedCount)
	}
}

// ---------------------------------------------------------------------------
// Render (ANSI-stripped substring assertions).
// ---------------------------------------------------------------------------

func representativeStates(now time.Time) []NodeState {
	return []NodeState{
		{Host: "devnet-02", Status: StatusDone},
		{Host: "devnet-03", Status: StatusRunning, HasProg: true,
			Prog: mkProg("ingest", 0.94, 40, 30), LastAdvance: now, Now: now},
		{Host: "devnet-07", Status: StatusRunning, HasProg: true,
			Prog: mkProg("merge", 0.61, 280, 22), LastAdvance: now, Now: now},
		{Host: "devnet-11", Status: StatusRunning, HasProg: true,
			Prog: mkProg("build", 0.28, 900, 8),
			// stalled: last advance 12s before Now, interval 3s -> >6s
			LastAdvance: now.Add(-12 * time.Second), Now: now},
		{Host: "devnet-14", Status: StatusStarting, Now: now},
		{Host: "devnet-19", Status: StatusFailed, RC: 137, HasProg: true, Prog: mkProg("merge", 0.44, -1, 5)},
	}
}

func TestRenderFrame(t *testing.T) {
	now := time.Unix(8_000_000, 0)
	vm := Aggregate(representativeStates(now), Options{
		Title: "DAN basechain generation", Interval: 3 * time.Second, AccountsTotal: 45000000,
	})
	frame := StripANSI(Render(vm, Options{Color: false, BarWidth: 20, Interval: 3 * time.Second}))

	mustContain(t, frame, "DAN basechain generation")
	mustContain(t, frame, "6 nodes")
	mustContain(t, frame, "45,000,000 accounts")
	mustContain(t, frame, "OVERALL")
	mustContain(t, frame, "phases")
	mustContain(t, frame, "devnet-14") // the starting node is the slowest (0%)
	mustContain(t, frame, "slowest devnet-14")
	mustContain(t, frame, "✓ done")
	mustContain(t, frame, "FAILED (rc 137)")
	mustContain(t, frame, "⚠ stalled 12s")
	mustContain(t, frame, "· starting")
	mustContain(t, frame, "done 1/6")
	mustContain(t, frame, "failed 1")
}

// TestRenderSampleFrame prints a full ANSI-stripped frame for the write-up.
// Run with `go test -run TestRenderSampleFrame -v` to capture it.
func TestRenderSampleFrame(t *testing.T) {
	now := time.Unix(9_000_000, 0)
	vm := Aggregate(representativeStates(now), Options{
		Title: "DAN basechain generation", Interval: 3 * time.Second, AccountsTotal: 45000000,
	})
	frame := StripANSI(Render(vm, Options{Color: false, BarWidth: 20, Interval: 3 * time.Second}))
	t.Logf("sample frame:\n%s\n", frame)
}

// ---------------------------------------------------------------------------
// helpers.
// ---------------------------------------------------------------------------

func statesByHost(tr *Tracker) map[string]NodeState {
	m := map[string]NodeState{}
	for _, s := range tr.States() {
		m[s.Host] = s
	}
	return m
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func mustContain(t *testing.T, haystack, needle string) {
	t.Helper()
	if !strings.Contains(haystack, needle) {
		t.Errorf("frame missing %q\n---\n%s\n---", needle, haystack)
	}
}
