package main

import (
	"context"
	"fmt"
	"time"

	tg "github.com/go-telegram/bot"
	"github.com/go-telegram/bot/models"
	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
	"github.com/victor/jetton-bomb-go-spam/pkg/streaming"
)

type observedKey struct{}
type observed struct {
	sample *streaming.Sample
	to     *address.Address
}

func observedFrom(ctx context.Context) *observed {
	v, _ := ctx.Value(observedKey{}).(*observed)
	return v
}

func destinationFor(ctx context.Context, pick func() *address.Address) *address.Address {
	if v := observedFrom(ctx); v != nil {
		return v.to
	}
	return pick()
}

func (r *Runtime) bindObserved(ctx context.Context, dst *address.Address, body *cell.Cell) {
	if r.stream == nil {
		return
	}
	if v := observedFrom(ctx); v != nil {
		r.stream.Bind(v.sample, dst, body)
	}
}

// watchTransfers is shared by every transfer mode. Only the once-per-second
// winner derives a recipient and waits for a subscription ACK; other sends do
// not acquire a lock, make an RPC, or wait for the streaming service.
func (r *Runtime) watchTransfers(cfg *pool.LoopConfig, run *SpamRun, pick func(*pool.Entry) *address.Address) {
	if r.stream == nil {
		return
	}
	r.streamLast.Store(run)
	cfg.Prepare = func(ctx context.Context, e *pool.Entry) context.Context {
		if !r.stream.Claim() {
			return ctx
		}
		to := pick(e)
		kind := "ton_transfer"
		asset := ""
		switch run.kind {
		case "jetton", "hamster", modeFatJetton, modeGasslessJetton:
			kind = "jetton_transfer"
			asset = r.jettonMinter.StringRaw()
		}
		s := r.stream.TrySample(ctx, streaming.Intent{RunID: run.startedAt.UnixNano(), Mode: run.kind,
			Kind: kind, Sender: e.Wallet.Address().StringRaw(), Recipient: to.StringRaw(),
			Asset: asset, Gasless: run.kind == modeGasslessGram || run.kind == modeGasslessJetton})
		if s == nil {
			return ctx
		}
		return context.WithValue(ctx, observedKey{}, &observed{sample: s, to: to})
	}
	cfg.Finish = func(ctx context.Context) {
		if v := observedFrom(ctx); v != nil {
			r.stream.EndAttempt(v.sample)
		}
	}
}

func (r *Runtime) streamLine(run *SpamRun) string {
	if r.stream == nil || !r.stream.Enabled() || r.streamMemory == nil {
		return ""
	}
	s := r.streamMemory.Snapshot(run.startedAt.UnixNano())
	s.Pending = uint64(r.stream.Pending(run.startedAt.UnixNano()))
	return formatDeliveryHTML(s)
}

func (r *Runtime) auditDeliveryLine(run *SpamRun) string {
	if r.stream == nil || !r.stream.Enabled() || r.streamMemory == nil {
		return ""
	}
	s := r.streamMemory.Snapshot(run.startedAt.UnixNano())
	return fmt.Sprintf("\nconfirmed p50 %s, p80 %s, p99 %s\nfinalized p50 %s, p80 %s, p99 %s",
		formatLatency(s.P50), formatLatency(s.P80), formatLatency(s.P99),
		formatLatency(s.FinalizedP50), formatLatency(s.FinalizedP80), formatLatency(s.FinalizedP99))
}

func (r *Runtime) auditTransferStatus(run *SpamRun, stats *pool.Stats, elapsed float64) string {
	return run.auditHeader + "\n" + auditStatLine(stats, elapsed) + r.auditDeliveryLine(run)
}

// formatDeliveryHTML follows the requests spoiler so delivery details stay
// collapsed until needed. The headline shows the p80 of confirmed samples.
func formatDeliveryHTML(s streaming.Snapshot) string {
	return fmt.Sprintf("\n<details><summary>delivery <b>p80 %s</b></summary>\n"+
		"<table bordered striped>\n"+
		"<thead><tr><th>Metric</th><th>Value</th><th>What it counts</th></tr></thead>\n<tbody>\n"+
		"<tr><td>confirmed</td><td>%s</td><td>matching confirmed actions</td></tr>\n"+
		"<tr><td>pending</td><td>%s</td><td>samples awaiting confirmation or finalization</td></tr>\n"+
		"<tr><td>not found</td><td>%s</td><td>no action within 90s</td></tr>\n"+
		"<tr><td>interrupted</td><td>%s</td><td>stream disconnected or switched off</td></tr>\n"+
		"<tr><td>send failed</td><td>%s</td><td>no external dispatched</td></tr>\n"+
		"<tr><td>no slots</td><td>%s</td><td>samples skipped at address limit</td></tr>\n"+
		"<tr><td>p50</td><td>%s</td><td>median confirmed delivery</td></tr>\n"+
		"<tr><td>p95</td><td>%s</td><td>confirmed delivery percentile</td></tr>\n"+
		"<tr><td>p99</td><td>%s</td><td>confirmed delivery percentile</td></tr>\n"+
		"<tr><td>finalized</td><td>%s</td><td>matching finalized actions</td></tr>\n"+
		"<tr><td>finalized not found</td><td>%s</td><td>confirmed, but no finalization within 90s</td></tr>\n"+
		"<tr><td>finalized p50</td><td>%s</td><td>median finalized delivery</td></tr>\n"+
		"<tr><td>finalized p80</td><td>%s</td><td>finalized delivery percentile</td></tr>\n"+
		"<tr><td>finalized p95</td><td>%s</td><td>finalized delivery percentile</td></tr>\n"+
		"<tr><td>finalized p99</td><td>%s</td><td>finalized delivery percentile</td></tr>\n"+
		"</tbody></table></details>",
		formatLatency(s.P80), formatInt(s.Confirmed), formatInt(s.Pending), formatInt(s.Missing),
		formatInt(s.Interrupted), formatInt(s.SendFailed), formatInt(s.NoSlot),
		formatLatency(s.P50), formatLatency(s.P95), formatLatency(s.P99),
		formatInt(s.Finalized), formatInt(s.FinalizedMissing),
		formatLatency(s.FinalizedP50), formatLatency(s.FinalizedP80),
		formatLatency(s.FinalizedP95), formatLatency(s.FinalizedP99))
}

func formatLatency(d time.Duration) string {
	if d == 0 {
		return "-"
	}
	return fmt.Sprintf("%.2fs", d.Seconds())
}

func (r *Runtime) handleStreaming(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	state := argAt(msgText(update.Message), 1)
	if r.stream == nil {
		reply(ctx, b, chat, "Streaming is not configured (set STREAMING_API_KEY and STREAMING_NETWORK)")
		return
	}
	switch state {
	case "on":
		r.stream.On()
	case "off":
		r.stream.Off()
	case "":
	default:
		reply(ctx, b, chat, "Usage: /streaming on|off")
		return
	}
	r.logger.Printf("[INFO] /streaming %s user=%s", state, userTag(update.Message.From))
	if state != "" {
		r.audit("streaming -> %s\nby: %s", state, userTag(update.Message.From))
	}
	value := "off"
	if r.stream.Enabled() {
		value = "connecting"
		if r.stream.Ready() {
			value = "on"
		}
	}
	reply(ctx, b, chat, fmt.Sprintf("Streaming: %s · subscribed addresses: %d/%d · max wait: 90s", value, r.stream.AddressCount(), r.stream.Limit()))
}
