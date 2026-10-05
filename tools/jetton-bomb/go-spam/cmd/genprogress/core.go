// Pure, network-free core of genprogress: schema types, per-node state
// tracking (incl. stall detection), aggregation into a ViewModel, and the
// ANSI dashboard renderer. Everything here is deterministic given its inputs,
// so it is unit-testable with no ssh / no clock — see core_test.go.
//
// The impure half (ssh puller, main loop, tty handling, signals) lives in
// main.go. It feeds Sample values into a Tracker, then renders Aggregate's
// ViewModel each tick.
package main

import (
	"encoding/json"
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

// ---------------------------------------------------------------------------
// progress.json schema (v1) — see PROGRESS_CONTRACT.md §1.
// ---------------------------------------------------------------------------

// Progress is one node's progress.json snapshot. Unknown fields are ignored,
// so forward-compatible additions on the C++ side (e.g. "seed") are harmless.
type Progress struct {
	Schema        int     `json:"schema"`
	Phase         string  `json:"phase"`
	PhaseName     string  `json:"phase_name"` // derive | build | merge | ingest | done
	PhaseIndex    int     `json:"phase_index"`
	PhaseCount    int     `json:"phase_count"`
	Done          int64   `json:"done"`
	Total         int64   `json:"total"`
	Unit          string  `json:"unit"`
	PhasePct      float64 `json:"phase_pct"`
	GlobalPct     float64 `json:"global_pct"` // overall weighted fraction, MONOTONIC 0..1
	Rate          float64 `json:"rate"`
	EtaSec        int64   `json:"eta_sec"` // seconds until the WHOLE run finishes; -1 if unknown
	ElapsedSec    int64   `json:"elapsed_sec"`
	Accounts      int64   `json:"accounts"`
	AccountsTotal int64   `json:"accounts_total"`
	Seq           int64   `json:"seq"` // monotonic tick counter (>=1) for stall detection
	TsUnix        int64   `json:"ts_unix"`
	DoneFlag      bool    `json:"done_flag"`
	Seed          string  `json:"seed"` // optional, not in the required schema; shown if present
}

// ParseProgress decodes progress.json bytes. Empty/whitespace input and
// malformed JSON both return an error; the caller treats that as "no progress".
func ParseProgress(raw string) (Progress, error) {
	if strings.TrimSpace(raw) == "" {
		return Progress{}, fmt.Errorf("empty progress")
	}
	var p Progress
	if err := json.Unmarshal([]byte(raw), &p); err != nil {
		return Progress{}, err
	}
	return p, nil
}

// ---------------------------------------------------------------------------
// Per-node status + observed state.
// ---------------------------------------------------------------------------

// NodeStatus is the terminal/liveness state of a node this tick.
type NodeStatus int

const (
	StatusStarting    NodeStatus = iota // reachable, no progress.json and no async.rc yet
	StatusRunning                       // progress.json present, async.rc absent
	StatusDone                          // async.rc == 0
	StatusFailed                        // async.rc != 0
	StatusUnreachable                   // ssh failed and we have no prior progress
)

func (s NodeStatus) String() string {
	switch s {
	case StatusStarting:
		return "starting"
	case StatusRunning:
		return "running"
	case StatusDone:
		return "done"
	case StatusFailed:
		return "failed"
	case StatusUnreachable:
		return "unreachable"
	default:
		return "?"
	}
}

// Sample is the raw per-tick observation for one host, produced by the puller.
type Sample struct {
	Host        string // map key / display fallback
	Label       string // optional override (e.g. "local")
	SSHFailed   bool   // the ssh transport itself failed (exit 255 / timeout / spawn error)
	RawProgress string // progress.json bytes (may be empty)
	RawRC       string // async.rc content (may be empty)
}

// NodeState is the accumulated view of one node across ticks. It carries the
// last-known progress plus the bookkeeping stall detection needs (LastSeq /
// LastAdvance vs Now). All fields exported so tests can build states directly.
type NodeState struct {
	Host    string
	Label   string
	Status  NodeStatus
	Prog    Progress
	HasProg bool
	RC      int
	HasRC   bool

	LastSeq     int64     // seq of the most recent progress we saw advance
	LastAdvance time.Time // wall time when LastSeq last changed
	Now         time.Time // time of the latest observation

	TickUnreachable bool // this tick's ssh failed (but we may still show last-known progress)
}

// DisplayLabel is the node's shown name (Label if set, else Host).
func (n NodeState) DisplayLabel() string {
	if n.Label != "" {
		return n.Label
	}
	return n.Host
}

func (n *NodeState) applyProgress(p Progress, now time.Time) {
	if !n.HasProg || p.Seq > n.LastSeq {
		n.LastSeq = p.Seq
		n.LastAdvance = now
	}
	n.Prog = p
	n.HasProg = true
}

// parseRC interprets async.rc content. Empty → (0,false) not-present. A clean
// integer → (v,true). Non-empty garbage is treated as not-yet-valid (ignored)
// so a partial write can never be mistaken for a failure.
func parseRC(raw string) (int, bool) {
	s := strings.TrimSpace(raw)
	if s == "" {
		return 0, false
	}
	v, err := strconv.Atoi(s)
	if err != nil {
		return 0, false
	}
	return v, true
}

// Tracker holds per-host NodeState across ticks, in first-seen order.
type Tracker struct {
	nodes map[string]*NodeState
	order []string
}

// NewTracker returns an empty Tracker.
func NewTracker() *Tracker {
	return &Tracker{nodes: map[string]*NodeState{}}
}

// Observe folds one Sample (observed at time now) into the tracked state.
// async.rc is authoritative for terminal state (done/failed). Otherwise the
// node is running (progress present), starting (reachable, no files), or
// unreachable (ssh failed and never produced progress). A tick whose ssh
// failed but which had prior progress keeps its last-known running state; its
// seq won't advance, so the stall detector flags it after 2×interval.
func (t *Tracker) Observe(s Sample, now time.Time) {
	n := t.nodes[s.Host]
	if n == nil {
		n = &NodeState{Host: s.Host}
		t.nodes[s.Host] = n
		t.order = append(t.order, s.Host)
	}
	if s.Label != "" {
		n.Label = s.Label
	}
	n.Now = now
	n.TickUnreachable = false

	// async.rc is authoritative for terminal state.
	if rc, ok := parseRC(s.RawRC); ok {
		n.HasRC = true
		n.RC = rc
		if rc == 0 {
			n.Status = StatusDone
		} else {
			n.Status = StatusFailed
		}
		if p, err := ParseProgress(s.RawProgress); err == nil {
			n.applyProgress(p, now)
		}
		return
	}
	n.HasRC = false

	// Live progress present → running.
	if p, err := ParseProgress(s.RawProgress); err == nil {
		n.applyProgress(p, now)
		n.Status = StatusRunning
		return
	}

	// No parsable progress this tick.
	if s.SSHFailed {
		n.TickUnreachable = true
		if !n.HasProg {
			n.Status = StatusUnreachable
		}
		// else: keep last-known Status (Running); stall detector handles it.
		return
	}
	// ssh ok but files missing: freshly starting, or a transient gap.
	if n.HasProg {
		n.Status = StatusRunning
	} else {
		n.Status = StatusStarting
	}
}

// States returns the tracked NodeStates in first-seen order (copies).
func (t *Tracker) States() []NodeState {
	out := make([]NodeState, 0, len(t.order))
	for _, h := range t.order {
		out = append(out, *t.nodes[h])
	}
	return out
}

// ---------------------------------------------------------------------------
// Aggregation -> ViewModel.
// ---------------------------------------------------------------------------

// Options carries render/aggregate knobs shared by both halves.
type Options struct {
	Title         string
	AccountsTotal int64         // header hint; 0 → take max from snapshots
	Interval      time.Duration // used for stall threshold (2×interval)
	Color         bool
	BarWidth      int // 0 → default 20
}

// NodeLine is one rendered node's derived display data.
type NodeLine struct {
	Host        string
	Label       string
	Status      NodeStatus
	Pct         float64 // fraction 0..1 shown on this node's bar
	PhaseName   string
	Stalled     bool
	StalledFor  int64 // seconds since last progress advance
	Unreachable bool  // latest tick's ssh failed
	IsSlowest   bool
	RC          int
}

// ViewModel is the fully-derived frame the renderer draws.
type ViewModel struct {
	Title         string
	NodeCount     int
	AccountsTotal int64
	Seed          string
	AggPct        float64 // slowest running/queued node's global_pct
	SlowestHost   string
	SlowestPhase  string
	Nodes         []NodeLine

	DoneCount        int
	FailedCount      int
	RunningCount     int
	StartingCount    int
	UnreachableCount int

	AllDone       bool
	AnyFailed     bool
	GenElapsedSec int64 // max elapsed_sec across nodes
}

// Aggregate derives a ViewModel from the tracked node states. The aggregate
// progress bar is driven by the SLOWEST running/queued node (min global_pct),
// and a running node whose progress seq has not advanced for > 2×interval is
// flagged stalled.
func Aggregate(states []NodeState, o Options) ViewModel {
	interval := o.Interval
	if interval <= 0 {
		interval = 3 * time.Second
	}
	vm := ViewModel{
		Title:         o.Title,
		NodeCount:     len(states),
		AccountsTotal: o.AccountsTotal,
	}

	lines := make([]NodeLine, len(states))
	slowIdx := -1
	slowPct := math.Inf(1)
	accTotal := o.AccountsTotal
	var seed string
	var genElapsed int64

	for i := range states {
		n := states[i]
		line := NodeLine{
			Host:      n.Host,
			Label:     n.DisplayLabel(),
			Status:    n.Status,
			PhaseName: n.Prog.PhaseName,
			RC:        n.RC,
		}
		if n.HasProg {
			if o.AccountsTotal == 0 && n.Prog.AccountsTotal > accTotal {
				accTotal = n.Prog.AccountsTotal
			}
			if seed == "" && n.Prog.Seed != "" {
				seed = n.Prog.Seed
			}
			if n.Prog.ElapsedSec > genElapsed {
				genElapsed = n.Prog.ElapsedSec
			}
		}

		switch n.Status {
		case StatusDone:
			line.Pct = 1.0
			line.PhaseName = "done"
			vm.DoneCount++
		case StatusFailed:
			line.Pct = progPct(n)
			vm.FailedCount++
		case StatusRunning:
			line.Pct = n.Prog.GlobalPct
			vm.RunningCount++
			if n.HasProg && !n.LastAdvance.IsZero() {
				if age := n.Now.Sub(n.LastAdvance); age > 2*interval {
					line.Stalled = true
					line.StalledFor = int64(age.Seconds())
				}
			}
			line.Unreachable = n.TickUnreachable
		case StatusStarting:
			line.Pct = 0
			vm.StartingCount++
		case StatusUnreachable:
			line.Pct = progPct(n)
			line.Unreachable = true
			vm.UnreachableCount++
		}
		lines[i] = line

		// The slowest node is picked over running + queued nodes (everything not terminal).
		if n.Status != StatusDone && n.Status != StatusFailed {
			if line.Pct < slowPct {
				slowPct = line.Pct
				slowIdx = i
			}
		}
	}

	if slowIdx >= 0 {
		lines[slowIdx].IsSlowest = true
		vm.SlowestHost = lines[slowIdx].Label
		vm.SlowestPhase = lines[slowIdx].PhaseName
		vm.AggPct = slowPct
	} else if vm.NodeCount > 0 {
		// No active nodes: everything terminal.
		if vm.DoneCount == vm.NodeCount {
			vm.AggPct = 1.0
			vm.SlowestPhase = "done"
		} else {
			vm.AggPct = float64(vm.DoneCount) / float64(vm.NodeCount)
		}
	}

	vm.Nodes = lines
	vm.AccountsTotal = accTotal
	vm.Seed = seed
	vm.GenElapsedSec = genElapsed
	vm.AllDone = vm.NodeCount > 0 && vm.DoneCount == vm.NodeCount
	vm.AnyFailed = vm.FailedCount > 0
	return vm
}

func progPct(n NodeState) float64 {
	if n.HasProg {
		return n.Prog.GlobalPct
	}
	return 0
}

// ---------------------------------------------------------------------------
// Rendering (ANSI dashboard).
// ---------------------------------------------------------------------------

const (
	ansiReset   = "\x1b[0m"
	ansiBold    = "\x1b[1m"
	ansiDim     = "\x1b[2m"
	ansiRed     = "\x1b[31m"
	ansiGreen   = "\x1b[32m"
	ansiYellow  = "\x1b[33m"
	ansiBlue    = "\x1b[34m"
	ansiMagenta = "\x1b[35m"
	ansiCyan    = "\x1b[36m"
	ansiBright  = "\x1b[92m" // bright green (done)
	ansiGray    = "\x1b[90m"
)

func phaseColor(name string) string {
	switch name {
	case "derive":
		return ansiCyan
	case "build":
		return ansiBlue
	case "merge":
		return ansiMagenta
	case "ingest":
		return ansiGreen
	case "done":
		return ansiBright
	default:
		return ansiGray
	}
}

func colorize(s, code string, on bool) string {
	if !on || code == "" {
		return s
	}
	return code + s + ansiReset
}

var ansiRE = regexp.MustCompile("\x1b\\[[0-9;]*m")

// StripANSI removes SGR escape sequences (for non-tty output and tests).
func StripANSI(s string) string { return ansiRE.ReplaceAllString(s, "") }

func visibleLen(s string) int { return utf8.RuneCountInString(StripANSI(s)) }

func rightPad(s string, w int) string {
	n := utf8.RuneCountInString(s)
	if n >= w {
		return s
	}
	return s + strings.Repeat(" ", w-n)
}

func clamp01(f float64) float64 {
	if math.IsNaN(f) || f < 0 {
		return 0
	}
	if f > 1 {
		return 1
	}
	return f
}

func pctStr(f float64) string { return fmt.Sprintf("%3.0f%%", clamp01(f)*100) }

// fmtDurSec renders a seconds count as e.g. "45s", "4m12s", "1h04m". -1 → "—".
func fmtDurSec(sec int64) string {
	if sec < 0 {
		return "—"
	}
	d := time.Duration(sec) * time.Second
	h := int(d / time.Hour)
	m := int(d/time.Minute) % 60
	s := int(d/time.Second) % 60
	switch {
	case h > 0:
		return fmt.Sprintf("%dh%02dm", h, m)
	case m > 0:
		return fmt.Sprintf("%dm%02ds", m, s)
	default:
		return fmt.Sprintf("%ds", s)
	}
}

func humanInt(n int64) string {
	neg := n < 0
	s := strconv.FormatInt(n, 10)
	if neg {
		s = s[1:]
	}
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if i > 0 && (len(s)-i)%3 == 0 {
			b.WriteByte(',')
		}
		b.WriteByte(s[i])
	}
	if neg {
		return "-" + b.String()
	}
	return b.String()
}

func renderBar(pct float64, width int, code string, color bool) string {
	if width <= 0 {
		width = 20
	}
	filled := int(math.Round(clamp01(pct) * float64(width)))
	if filled < 0 {
		filled = 0
	}
	if filled > width {
		filled = width
	}
	fill := colorize(strings.Repeat("█", filled), code, color)
	empty := colorize(strings.Repeat("░", width-filled), ansiGray, color)
	return "[" + fill + empty + "]"
}

// Render draws the full dashboard frame as a string (no trailing newline).
func Render(vm ViewModel, o Options) string {
	color := o.Color
	barW := o.BarWidth
	if barW <= 0 {
		barW = 20
	}

	hostCol := len("OVERALL")
	for _, n := range vm.Nodes {
		if l := utf8.RuneCountInString(n.Label); l > hostCol {
			hostCol = l
		}
	}

	// Title.
	title := vm.Title
	if title == "" {
		title = "DAN basechain generation"
	}
	titleLine := fmt.Sprintf("%s  ·  %d nodes", title, vm.NodeCount)
	if vm.AccountsTotal > 0 {
		titleLine += fmt.Sprintf("  ·  %s accounts", humanInt(vm.AccountsTotal))
	}
	if vm.Seed != "" {
		titleLine += fmt.Sprintf("  ·  seed %s", vm.Seed)
	}
	titleLine = colorize(titleLine, ansiBold, color)

	// Overall aggregate bar (slowest-driven).
	slowPhase := vm.SlowestPhase
	if slowPhase == "" {
		slowPhase = "queued"
	}
	overall := fmt.Sprintf("%s %s %s",
		colorize(rightPad("OVERALL", hostCol), ansiBold, color),
		renderBar(vm.AggPct, barW, phaseColor(vm.SlowestPhase), color),
		pctStr(vm.AggPct),
	)
	if vm.SlowestHost != "" {
		overall += colorize(fmt.Sprintf("   slowest %s (%s)", vm.SlowestHost, slowPhase), ansiDim, color)
	}

	legend := renderLegend(color)

	// Node lines.
	nodeLines := make([]string, len(vm.Nodes))
	for i, n := range vm.Nodes {
		nodeLines[i] = renderNode(n, hostCol, barW, color)
	}

	footer := fmt.Sprintf("done %d/%d · running %d · starting %d · failed %d",
		vm.DoneCount, vm.NodeCount, vm.RunningCount, vm.StartingCount, vm.FailedCount)
	if vm.UnreachableCount > 0 {
		footer += fmt.Sprintf(" · unreachable %d", vm.UnreachableCount)
	}
	footer += fmt.Sprintf("    gen %s", fmtDurSec(vm.GenElapsedSec))
	footer = colorize(footer, ansiDim, color)

	// Rule width = widest visible line.
	width := 0
	for _, l := range append([]string{titleLine, overall, legend, footer}, nodeLines...) {
		if v := visibleLen(l); v > width {
			width = v
		}
	}
	rule := colorize(strings.Repeat("─", width), ansiGray, color)

	var b strings.Builder
	b.WriteString(titleLine)
	b.WriteByte('\n')
	b.WriteString(rule)
	b.WriteByte('\n')
	b.WriteString(overall)
	b.WriteByte('\n')
	b.WriteString(legend)
	b.WriteByte('\n')
	b.WriteString(rule)
	b.WriteByte('\n')
	for _, l := range nodeLines {
		b.WriteString(l)
		b.WriteByte('\n')
	}
	b.WriteString(rule)
	b.WriteByte('\n')
	b.WriteString(footer)
	return b.String()
}

func renderLegend(color bool) string {
	dot := func(name string) string {
		return colorize("●", phaseColor(name), color) + " " + name
	}
	return "phases   " + dot("derive") + "  " + dot("build") + "  " + dot("merge") + "  " + dot("ingest") +
		"    " + colorize("✓", ansiBright, color) + " done  " + colorize("✗", ansiRed, color) + " failed"
}

func renderNode(n NodeLine, hostCol, barW int, color bool) string {
	label := rightPad(n.Label, hostCol)
	if n.IsSlowest {
		label = colorize(label, ansiBold, color)
	}

	var barCode string
	switch n.Status {
	case StatusDone:
		barCode = phaseColor("done")
	case StatusFailed:
		barCode = ansiRed
	default:
		barCode = phaseColor(n.PhaseName)
	}
	bar := renderBar(n.Pct, barW, barCode, color)

	seg := statusSegment(n, color)
	return fmt.Sprintf("%s %s %s  %s", label, bar, pctStr(n.Pct), seg)
}

func statusSegment(n NodeLine, color bool) string {
	switch n.Status {
	case StatusDone:
		return colorize("✓ done", ansiBright, color)
	case StatusFailed:
		return colorize(fmt.Sprintf("✗ FAILED (rc %d)", n.RC), ansiRed+ansiBold, color)
	case StatusStarting:
		return colorize("· starting", ansiDim, color)
	case StatusUnreachable:
		return colorize("⚠ unreachable", ansiYellow, color)
	default: // running
		phase := n.PhaseName
		if phase == "" {
			phase = "running"
		}
		seg := colorize(phase, phaseColor(n.PhaseName), color)
		if n.Stalled {
			seg += "  " + colorize(fmt.Sprintf("⚠ stalled %ds", n.StalledFor), ansiYellow, color)
		}
		if n.Unreachable {
			seg += "  " + colorize("(unreachable)", ansiDim, color)
		}
		if n.IsSlowest {
			seg += "  " + colorize("‹slowest›", ansiBold, color)
		}
		return seg
	}
}

// ---------------------------------------------------------------------------
// Hosts file parsing (counted-hosts OR plain list) — see PROGRESS_CONTRACT.md §2.
// ---------------------------------------------------------------------------

var intLineRE = regexp.MustCompile(`^[0-9]+$`)

// ParseHostsFile extracts hostnames from a counted-hosts file (line 1 an
// integer count to skip) OR a plain newline list. Blank lines and '#' comments
// are dropped; hosts are de-duplicated preserving first-seen order.
func ParseHostsFile(data string) []string {
	var raw []string
	for _, ln := range strings.Split(data, "\n") {
		ln = strings.TrimSpace(strings.TrimRight(ln, "\r"))
		if ln == "" || strings.HasPrefix(ln, "#") {
			continue
		}
		raw = append(raw, ln)
	}
	if len(raw) == 0 {
		return nil
	}
	// If the first meaningful line is a bare integer, it's the count → skip it.
	if intLineRE.MatchString(raw[0]) {
		raw = raw[1:]
	}
	seen := map[string]bool{}
	out := make([]string, 0, len(raw))
	for _, h := range raw {
		if seen[h] {
			continue
		}
		seen[h] = true
		out = append(out, h)
	}
	return out
}
