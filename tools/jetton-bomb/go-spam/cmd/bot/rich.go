package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"

	tg "github.com/go-telegram/bot"

	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
)

// sendRichHTML posts a rich-formatted HTML message to a chat via Telegram's
// /sendRichMessage endpoint. The go-telegram/bot v1.20 library doesn't wrap
// this method yet (rich messages are a newer Bot-API feature), so we hit the
// API directly with a minimal JSON envelope.
//
// On non-2xx response, returns the body as an error so the caller can log the
// reason. The caller is responsible for falling back to plain SendMessage if
// the bot is on a server that doesn't accept rich messages yet.
func (r *Runtime) sendRichHTML(ctx context.Context, chatID int64, html string) error {
	body, err := json.Marshal(map[string]any{
		"chat_id": chatID,
		"rich_message": map[string]string{
			"html": html,
		},
	})
	if err != nil {
		return err
	}
	url := "https://api.telegram.org/bot" + r.botToken + "/sendRichMessage"
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode/100 != 2 {
		buf, _ := io.ReadAll(io.LimitReader(resp.Body, 2048))
		return fmt.Errorf("sendRichMessage %d: %s", resp.StatusCode, string(buf))
	}
	_, _ = io.Copy(io.Discard, resp.Body)
	return nil
}

// sendStatus tries the rich HTML path first; on failure (e.g. the bot is on a
// Telegram instance that doesn't accept rich messages yet) it strips the HTML
// tags and falls back to a regular SendMessage so we still get a readable
// update through.
func (r *Runtime) sendStatus(ctx context.Context, b *tg.Bot, chat int64, html string) {
	if err := r.sendRichHTML(ctx, chat, html); err == nil {
		return
	} else {
		r.logger.Printf("[WARN] sendRichMessage failed, falling back: %v", err)
	}
	_, _ = b.SendMessage(ctx, &tg.SendMessageParams{
		ChatID: chat,
		Text:   stripHTML(html),
	})
}

// stripHTML is a tiny, intentionally-dumb tag stripper used only for the
// plain-text fallback path. Inserts structural whitespace for the table/row
// tags we emit so columns don't all collapse onto one line.
func stripHTML(s string) string {
	for _, h := range []string{"</h1>", "</h2>", "</h3>", "</h4>", "</h5>", "</h6>"} {
		s = strings.ReplaceAll(s, h, "\n")
	}
	s = strings.ReplaceAll(s, "</tr>", "\n")
	s = strings.ReplaceAll(s, "</caption>", "\n")
	s = strings.ReplaceAll(s, "</td>", " ")
	s = strings.ReplaceAll(s, "</th>", " ")
	var out []byte
	depth := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '<':
			depth++
		case c == '>' && depth > 0:
			depth--
		case depth == 0:
			out = append(out, c)
		}
	}
	return string(out)
}

// formatStatusHTML builds a spam loop status message as rich-HTML with a
// deliberate visual hierarchy: H1 carries the send rate (the headline number),
// H2 carries the failure rate paired with the accepted-share in the current
// window, and the rest (throttled, inflight, cumulative totals, lifetime
// acceptance) is de-emphasised in an italic tail.
// Snap is a point-in-time copy of all counters in pool.Stats. Used as the
// "previous tick" baseline so formatStatusHTML can report per-window rates.
type Snap struct {
	Sent, Failed, Throttled, Skipped, Retried uint64
	Send0, Seqno1, Send1, Seqno2, Send2       uint64
	BatchSend, BatchRetry, Batches            uint64
}

func takeSnap(s *pool.Stats) Snap {
	return Snap{
		Sent:       s.Sent.Load(),
		Failed:     s.Failed.Load(),
		Throttled:  s.Throttled.Load(),
		Skipped:    s.Skipped.Load(),
		Retried:    s.Retried.Load(),
		Send0:      s.Send0.Load(),
		Seqno1:     s.Seqno1.Load(),
		Send1:      s.Send1.Load(),
		Seqno2:     s.Seqno2.Load(),
		Send2:      s.Send2.Load(),
		BatchSend:  s.BatchSend.Load(),
		BatchRetry: s.BatchRetry.Load(),
		Batches:    s.Batches.Load(),
	}
}

func formatStatusHTML(kind string, s *pool.Stats, last Snap, periodSec float64) string {
	if periodSec <= 0 {
		periodSec = 1
	}
	cur := takeSnap(s)
	dSent := float64(cur.Sent - last.Sent)
	dFailed := float64(cur.Failed - last.Failed)
	dThrottled := float64(cur.Throttled - last.Throttled)
	dSkipped := float64(cur.Skipped - last.Skipped)
	dRetried := float64(cur.Retried - last.Retried)

	sRate := dSent / periodSec
	fRate := dFailed / periodSec
	tRate := dThrottled / periodSec
	skRate := dSkipped / periodSec
	rRate := dRetried / periodSec

	winPct := 0.0
	if dSent+dFailed > 0 {
		winPct = dSent / (dSent + dFailed) * 100
	}
	overallPct := 0.0
	if cur.Sent+cur.Failed > 0 {
		overallPct = float64(cur.Sent) / float64(cur.Sent+cur.Failed) * 100
	}
	// Window-based retry share: what fraction of CURRENT sent-rate went
	// through the retry path. retried_rate / sent_rate matches the operator
	// intuition "of N/s I'm delivering, how many were rescued by retry" —
	// more actionable than the cumulative ratio, which lags behind during
	// rate changes. Can exceed 100% if most retries also fail; that itself
	// is a useful signal.
	retriedPct := 0.0
	if sRate > 0 {
		retriedPct = rRate / sRate * 100
	}

	// Kind goes inside H1 (not a separate <b>) to remove one block-margin
	// between header lines — Telegram gives every block element its own
	// top/bottom gap, so each line we collapse saves real vertical space.
	head := fmt.Sprintf(
		"<h1>%s · %.0f/s sent</h1>\n<h2>%.0f/s failed · %.1f%% accepted</h2>",
		kind, sRate, fRate, winPct,
	)
	return head + formatTotalsHTML(cur, overallPct, rRate, retriedPct, tRate, skRate, s.Inflight.Load()) +
		formatRequestsBreakdownHTML(cur, last, periodSec)
}

// formatTotalsHTML wraps the cumulative-counter block in a collapsed <details>
// so the at-a-glance message stays as just the H1/H2 headline. The summary
// shows lifetime sent count and overall acceptance — enough to decide whether
// to expand. The body is a monospace right-aligned table in the order the
// operator scans: totals → retried → failed → throttled → inflight.
func formatTotalsHTML(cur Snap, overallPct, rRate, retriedPct, tRate, skRate float64, inflight int64) string {
	body := fmt.Sprintf(
		"%9s  %s sent (%.1f%% overall)\n%9s  %s (%.0f/s, %.1f%%)\n%9s  %s\n%9s  %s (%.0f/s)\n%9s  %s (%.0f/s)\n%9s  %d",
		"totals", formatInt(cur.Sent), overallPct,
		"retried", formatInt(cur.Retried), rRate, retriedPct,
		"failed", formatInt(cur.Failed),
		"throttled", formatInt(cur.Throttled), tRate,
		"skipped", formatInt(cur.Skipped), skRate,
		"inflight", inflight,
	)
	if cur.BatchSend > 0 {
		body += fmt.Sprintf("\n%9s  %s accepted externals", "batches", formatInt(cur.Batches))
	}
	return fmt.Sprintf(
		"\n<details><summary>totals <b>%s</b> sent (%.1f%%)</summary>\n<pre>%s</pre></details>",
		formatInt(cur.Sent), overallPct, body,
	)
}

// requestSteps describes the five pipeline phases shown inside the collapsible
// "requests" block. The display order matches the temporal order inside runOne:
// each event traverses one of two opening paths (Send0 if cache is warm,
// or Seqno1+Send1 if cold), and on failure adds Seqno2+Send2 for the retry.
var requestSteps = []struct {
	name string
	desc string
}{
	{"Send0", "first send with warm cache (no preceding fetch)"},
	{"Seqno1", "first-attempt seqno fetches (pair modes may use 2+)"},
	{"Send1", "first send after one or more Seqno1 fetches"},
	{"Seqno2", "retry-path seqno fetches (pair modes may use 2+)"},
	{"Send2", "retry send after a successful Seqno2"},
	{"BatchSend", "first batch external RPC (many gasless requests)"},
	{"BatchRetry", "same batch external retried through the 500ms limiter"},
}

// formatRequestsBreakdownHTML renders the per-phase backend-RPS breakdown as
// a collapsed <details> block: the summary shows the totaled requests/s, and
// expanding reveals a per-step table that names each phase and explains
// what it counts. Mirrors FormatBreakdownHTML's shape (errors spoiler) so
// the two collapsible sections feel consistent.
func formatRequestsBreakdownHTML(cur, last Snap, periodSec float64) string {
	if periodSec <= 0 {
		periodSec = 1
	}
	rates := []float64{
		float64(cur.Send0-last.Send0) / periodSec,
		float64(cur.Seqno1-last.Seqno1) / periodSec,
		float64(cur.Send1-last.Send1) / periodSec,
		float64(cur.Seqno2-last.Seqno2) / periodSec,
		float64(cur.Send2-last.Send2) / periodSec,
		float64(cur.BatchSend-last.BatchSend) / periodSec,
		float64(cur.BatchRetry-last.BatchRetry) / periodSec,
	}
	total := 0.0
	for _, r := range rates {
		total += r
	}
	var sb strings.Builder
	fmt.Fprintf(&sb, "\n<details><summary>requests <b>%.0f/s</b></summary>\n", total)
	sb.WriteString("<table bordered striped>\n")
	sb.WriteString("<thead><tr><th>Step</th><th>RPS</th><th>What it counts</th></tr></thead>\n<tbody>\n")
	for i, step := range requestSteps {
		fmt.Fprintf(&sb, "<tr><td>%s</td><td>%.0f/s</td><td>%s</td></tr>\n",
			step.name, rates[i], step.desc)
	}
	sb.WriteString("</tbody></table></details>")
	return sb.String()
}

// formatInt inserts thousands separators so 4537157 reads as 4 537 157.
func formatInt(n uint64) string {
	if n < 1000 {
		return fmt.Sprintf("%d", n)
	}
	s := fmt.Sprintf("%d", n)
	var out []byte
	for i, c := range []byte(s) {
		if i > 0 && (len(s)-i)%3 == 0 {
			out = append(out, ' ')
		}
		out = append(out, c)
	}
	return string(out)
}
