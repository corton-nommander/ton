package main

import (
	"fmt"
	"log"
	"regexp"
	"sort"
	"strings"
	"sync"
)

var (
	reIPPort = regexp.MustCompile(`\d+\.\d+\.\d+\.\d+:\d+`)
	reTX     = regexp.MustCompile(`[A-Fa-f0-9]{64}`)
	reBlock  = regexp.MustCompile(`\(-?\d+,[0-9a-f]+,\d+\)`)
)

// normalizeErr strips variable fields (IP:port, tx hash, block coords) and caps
// the length so semantically-identical errors hash to the same bucket key.
// Placeholders use [X] form (not <X>) so the keys can be dropped into HTML
// payloads without escaping.
func normalizeErr(s string) string {
	s = reBlock.ReplaceAllString(s, "([W],[SHARD],[SEQ])")
	s = reTX.ReplaceAllString(s, "[TX]")
	s = reIPPort.ReplaceAllString(s, "[NODE]")
	s = strings.ReplaceAll(s, "\n", " | ")

	// Strip wordy boilerplate that wraps almost every lite-server rejection.
	s = strings.ReplaceAll(s, "cannot apply external message to current state : ", "")
	s = strings.ReplaceAll(s, "External message was not accepted", "rejected")
	s = strings.ReplaceAll(s, "external message was not accepted", "rejected")
	s = strings.ReplaceAll(s, ": cannot run message on account: ", ": ")
	s = strings.ReplaceAll(s, "inbound external message rejected by transaction [TX]", "tx [TX]")
	s = strings.ReplaceAll(s, ", err: context deadline exceeded", " (ctx-deadline)")
	s = strings.ReplaceAll(s, ", err: context canceled", " (ctx-cancel)")
	s = strings.ReplaceAll(s, "lite server error, ", "lite ")

	// Drop the VM Log tail — the exitcode= line is what carries the signal.
	if idx := strings.Index(s, " | VM Log"); idx > 0 {
		s = s[:idx]
	}

	if len(s) > 180 {
		s = s[:180]
	}
	return s
}

// ErrStack tallies normalized error messages by type. One instance per SpamRun.
// SnapshotDelta returns counts since the previous call so OnTick can report
// per-window rates.
type ErrStack struct {
	mu       sync.Mutex
	totals   map[string]uint64
	lastTick map[string]uint64
}

func NewErrStack() *ErrStack {
	return &ErrStack{
		totals:   make(map[string]uint64),
		lastTick: make(map[string]uint64),
	}
}

// Record buckets one error event. Safe for concurrent callers.
func (e *ErrStack) Record(phase string, errMsg string) {
	key := phase + ": " + normalizeErr(errMsg)
	e.mu.Lock()
	e.totals[key]++
	e.mu.Unlock()
}

// ErrCount is one bucket in the histogram.
type ErrCount struct {
	Key   string
	Count uint64
}

// SnapshotDelta returns the top-n entries with counts accumulated since the
// previous SnapshotDelta call, along with the total delta. Advances the tick
// baseline. Pass n<=0 to return all entries.
func (e *ErrStack) SnapshotDelta(n int) ([]ErrCount, uint64) {
	e.mu.Lock()
	delta := make(map[string]uint64, len(e.totals))
	var total uint64
	for k, v := range e.totals {
		d := v - e.lastTick[k]
		if d > 0 {
			delta[k] = d
			total += d
		}
	}
	e.lastTick = make(map[string]uint64, len(e.totals))
	for k, v := range e.totals {
		e.lastTick[k] = v
	}
	e.mu.Unlock()
	return topN(delta, n), total
}

func topN(m map[string]uint64, n int) []ErrCount {
	out := make([]ErrCount, 0, len(m))
	for k, v := range m {
		out = append(out, ErrCount{Key: k, Count: v})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Count > out[j].Count })
	if n > 0 && len(out) > n {
		out = out[:n]
	}
	return out
}

// FormatBreakdownHTML formats a top-N slice as a Telegram rich-HTML <details>
// block: collapsed by default with the error total visible in the summary,
// expanded on tap with a monospace <pre> table. Empty slice → empty string so
// callers can safely concatenate.
func FormatBreakdownHTML(entries []ErrCount, total uint64, periodSec float64) string {
	if len(entries) == 0 {
		return ""
	}
	var sb strings.Builder
	if periodSec > 0 {
		fmt.Fprintf(&sb, "\n<details><summary>errors <b>%d</b> (<i>%.0f/s</i>)</summary>\n<pre>", total, float64(total)/periodSec)
	} else {
		fmt.Fprintf(&sb, "\n<details><summary>errors <b>%d</b></summary>\n<pre>", total)
	}
	for _, e := range entries {
		fmt.Fprintf(&sb, "%7d  %s\n", e.Count, htmlEscape(e.Key))
	}
	sb.WriteString("</pre></details>")
	return sb.String()
}

// htmlEscape replaces the three HTML-special characters. Only `&` `<` `>` need
// escaping in the rich-HTML payload — quotes don't appear in attribute values
// here and `[` `]` are not special.
func htmlEscape(s string) string {
	s = strings.ReplaceAll(s, "&", "&amp;")
	s = strings.ReplaceAll(s, "<", "&lt;")
	s = strings.ReplaceAll(s, ">", "&gt;")
	return s
}

// WriteSnapshot dumps the same breakdown to a *log.Logger target. Used to keep
// errors.log as a rolling stack of per-window snapshots instead of the per-error
// torrent it used to be.
func WriteSnapshot(lg *log.Logger, kind string, entries []ErrCount, total uint64, periodSec float64) {
	if lg == nil || len(entries) == 0 {
		return
	}
	lg.Printf("=== %s window=%.0fs total=%d ===", kind, periodSec, total)
	for _, e := range entries {
		lg.Printf("%10d %s", e.Count, e.Key)
	}
}
