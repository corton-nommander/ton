package main

import (
	"context"
	"fmt"
	"io"
	"math/big"
	mathrand "math/rand"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	tg "github.com/go-telegram/bot"
	"github.com/go-telegram/bot/models"
	"github.com/xssnick/tonutils-go/address"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
	"github.com/victor/jetton-bomb-go-spam/pkg/drain"
	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
)

func reply(ctx context.Context, b *tg.Bot, chatID int64, text string) {
	_, err := b.SendMessage(ctx, &tg.SendMessageParams{ChatID: chatID, Text: text})
	if err != nil {
		// best-effort; reply errors only log
	}
}

// replyHTML sends with ParseMode HTML, for the few messages that carry links. The plain reply() above is
// NOT switched to HTML on purpose: a dozen usage strings contain <freq>/<n>/<tps>, which HTML mode would
// reject as unknown tags — and reply() swallows send errors, so those messages would vanish silently.
// Anything passed here must already be HTML-safe.
func replyHTML(ctx context.Context, b *tg.Bot, chatID int64, html string) {
	_, err := b.SendMessage(ctx, &tg.SendMessageParams{
		ChatID:             chatID,
		Text:               html,
		ParseMode:          models.ParseModeHTML,
		LinkPreviewOptions: &models.LinkPreviewOptions{IsDisabled: ptrTrue()},
	})
	if err != nil {
		// best-effort, same contract as reply()
	}
}

func ptrTrue() *bool { v := true; return &v }

func userTag(u *models.User) string {
	if u == nil {
		return "unknown"
	}
	if u.Username != "" {
		return fmt.Sprintf("%s(%d)", u.Username, u.ID)
	}
	return fmt.Sprintf("%s(%d)", u.FirstName, u.ID)
}

func msgText(m *models.Message) string {
	if m == nil {
		return ""
	}
	return m.Text
}

func parsePositiveInt(s string) (int, bool) {
	n, err := strconv.Atoi(s)
	if err != nil || n <= 0 {
		return 0, false
	}
	return n, true
}

func argAt(text string, idx int) string {
	parts := strings.Fields(strings.TrimSpace(text))
	if idx >= len(parts) {
		return ""
	}
	return parts[idx]
}

// /start <tps>
// /help — the same cheatsheet + status block /start prints with no argument. Exists because /start
// doubles as the static-TPS trigger, so the help text is easy to miss.
func (r *Runtime) handleHelp(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	r.logger.Printf("[INFO] /help user=%s", userTag(update.Message.From))
	reply(ctx, b, update.Message.Chat.ID, r.buildIntroMessage())
}

// /status — only the status block (backend, timeout, running loops, relayers), no cheatsheet.
func (r *Runtime) handleStatus(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	r.logger.Printf("[INFO] /status user=%s", userTag(update.Message.From))
	reply(ctx, b, update.Message.Chat.ID, r.buildStatusBlock())
}

func (r *Runtime) handleStart(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /start command user=%s text=%q", user, msgText(update.Message))
	tpsArg := argAt(msgText(update.Message), 1)
	if tpsArg == "" {
		reply(ctx, b, chat, r.buildIntroMessage())
		return
	}
	tps, ok := parsePositiveInt(tpsArg)
	if !ok {
		reply(ctx, b, chat, "TPS must be a positive integer. Usage: /start <tps>")
		return
	}
	target := r.shardAddrs[0]
	reply(ctx, b, chat, fmt.Sprintf(
		"Running TPS on 1 shard\nTarget: %s\nTarget TPS: %d\nTON: %s",
		target.String(), tps, RunAmountPerShardTON,
	))
	if err := r.sendStaticTpsRun(ctx, 0, tps); err != nil {
		r.logger.Printf("[ERROR] /start send err=%v", err)
		reply(ctx, b, chat, fmt.Sprintf("run failed: %v", err))
		return
	}
	r.staticRun.Store(&StaticRun{tps: tps, shards: 1, startedAt: time.Now()})
	r.logger.Printf("[INFO] /start ok user=%s tps=%d target=%s", user, tps, target.String())
	r.audit("start static tps (1 shard)\nby: %s\ntps: %d\ntarget: %s", user, tps, target.String())
	reply(ctx, b, chat, "run sent")
}

// /stop
// /stop — the panic button: stop EVERY running load at once (whatever pool mode is active, plus both
// static-TPS runs) and report what was actually stopped. The per-mode /*_stop handlers still exist for
// stopping one thing; this is the one to reach for when you just want silence.
func (r *Runtime) handleStopAll(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /stop (all) user=%s", user)

	var stopped []string

	// 1) Whatever pool-backed loop is running (jetton / gram / gassless / fat / hamster / external).
	if run := r.poolRun.Load(); run != nil {
		run.cancel()
		elapsed := time.Since(run.startedAt).Seconds()
		sent := run.stats.Sent.Load()
		failed := run.stats.Failed.Load()
		avg := 0.0
		if elapsed > 0 {
			avg = float64(sent) / elapsed
		}
		stopped = append(stopped, fmt.Sprintf("%s — sent %d, failed %d, %.1f/s over %.0fs",
			run.kind, sent, failed, avg, elapsed))
		r.audit("stop %s spam (via /stop)\nby: %s\nsent %d, failed %d, %.0f/s, %.0fs",
			run.kind, user, sent, failed, avg, elapsed)
	}

	// 2) Static TPS on shard 0. On-chain contracts keep running until the stop message lands, so report
	//    a send failure rather than claiming success.
	if r.staticRun.Load() != nil {
		if err := r.sendStaticTpsStop(ctx, 0); err != nil {
			r.logger.Printf("[ERROR] /stop static err=%v", err)
			stopped = append(stopped, fmt.Sprintf("static (1 shard) — STOP FAILED: %v", err))
		} else {
			r.staticRun.Store(nil)
			stopped = append(stopped, "static (1 shard) — stop sent")
			r.audit("stop static tps (1 shard, via /stop)\nby: %s", user)
		}
	}

	// 3) Static TPS on all 16 shards.
	if r.shardsRun.Load() != nil {
		if err := r.sendStaticTpsStopAll(ctx); err != nil {
			r.logger.Printf("[ERROR] /stop shards err=%v", err)
			stopped = append(stopped, fmt.Sprintf("static (%d shards) — STOP FAILED: %v", len(r.shardAddrs), err))
		} else {
			r.shardsRun.Store(nil)
			stopped = append(stopped, fmt.Sprintf("static (%d shards) — stop sent", len(r.shardAddrs)))
			r.audit("stop static tps (%d shards, via /stop)\nby: %s", len(r.shardAddrs), user)
		}
	}

	if len(stopped) == 0 {
		reply(ctx, b, chat, "Nothing was running — nothing to stop.")
		return
	}
	reply(ctx, b, chat, "Stopped:\n• "+strings.Join(stopped, "\n• "))
}

// /static_stop — stop the single-shard StaticTps run (what /stop used to do).
func (r *Runtime) handleStaticStop(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /static_stop user=%s", user)
	target := r.shardAddrs[0]
	reply(ctx, b, chat, fmt.Sprintf("Stopping TPS\nTarget: %s", target.String()))
	if err := r.sendStaticTpsStop(ctx, 0); err != nil {
		r.logger.Printf("[ERROR] /static_stop send err=%v", err)
		reply(ctx, b, chat, fmt.Sprintf("stop failed: %v", err))
		return
	}
	r.staticRun.Store(nil)
	r.audit("stop static tps (1 shard)\nby: %s\ntarget: %s", user, target.String())
	reply(ctx, b, chat, "stop sent")
}

// /shards <tps>
func (r *Runtime) handleShards(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /shards user=%s text=%q", user, msgText(update.Message))
	tpsArg := argAt(msgText(update.Message), 1)
	if tpsArg == "" {
		reply(ctx, b, chat, "Usage: /shards <tps>  (TPS is per contract; spam runs on all 16 shards)")
		return
	}
	tps, ok := parsePositiveInt(tpsArg)
	if !ok {
		reply(ctx, b, chat, "TPS must be a positive integer. Usage: /shards <tps>")
		return
	}
	totalTps := tps * len(r.shardAddrs)
	totalTon, _ := strconv.ParseFloat(RunAmountPerShardTON, 64)
	reply(ctx, b, chat, fmt.Sprintf(
		"Running TPS on %d shards\nTarget TPS per contract: %d (total ~%d)\nTON per contract: %s (total %.0f)",
		len(r.shardAddrs), tps, totalTps, RunAmountPerShardTON, totalTon*float64(len(r.shardAddrs)),
	))
	if err := r.sendStaticTpsRunAll(ctx, tps); err != nil {
		r.logger.Printf("[ERROR] /shards send err=%v", err)
		reply(ctx, b, chat, fmt.Sprintf("run failed: %v", err))
		return
	}
	r.shardsRun.Store(&StaticRun{tps: tps, shards: len(r.shardAddrs), startedAt: time.Now()})
	r.logger.Printf("[INFO] /shards ok user=%s tps=%d shards=%d", user, tps, len(r.shardAddrs))
	r.audit("start static tps (%d shards)\nby: %s\ntps/contract: %d (total ~%d)",
		len(r.shardAddrs), user, tps, totalTps)
	reply(ctx, b, chat, "run sent")
}

// /shards_stop
func (r *Runtime) handleShardsStop(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /shards_stop user=%s", user)
	reply(ctx, b, chat, fmt.Sprintf("Stopping TPS on %d shards", len(r.shardAddrs)))
	if err := r.sendStaticTpsStopAll(ctx); err != nil {
		r.logger.Printf("[ERROR] /shards_stop err=%v", err)
		reply(ctx, b, chat, fmt.Sprintf("stop failed: %v", err))
		return
	}
	r.shardsRun.Store(nil)
	r.audit("stop static tps (%d shards)\nby: %s", len(r.shardAddrs), user)
	reply(ctx, b, chat, "stop sent")
}

// /timeout NUMBER — set per-send timeout in ms.
func (r *Runtime) handleTimeout(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	arg := argAt(msgText(update.Message), 1)
	r.logger.Printf("[INFO] /timeout user=%s arg=%q", user, arg)

	if arg == "" {
		reply(ctx, b, chat, fmt.Sprintf("Current timeout: %dms\nUsage: /timeout <ms>", r.timeoutMs.Load()))
		return
	}
	n, ok := parsePositiveInt(arg)
	if !ok {
		reply(ctx, b, chat, "timeout must be a positive integer (ms). Usage: /timeout <ms>")
		return
	}
	r.timeoutMs.Store(int64(n))
	r.logger.Printf("[INFO] timeout set user=%s ms=%d", user, n)
	r.audit("timeout set to %dms\nby: %s", n, user)
	reply(ctx, b, chat, fmt.Sprintf("Timeout set to %dms", n))
}

// /initmode [0|1] — set the jetton transfer internal_init_mode carried in customPayload.
// 0 = full state_init (classic; deploys the recipient if absent). 1 = bare (no state_init — cheaper;
// valid because the pool's jetton wallets are pre-deployed in-chain).
func (r *Runtime) handleInitMode(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	arg := argAt(msgText(update.Message), 1)
	r.logger.Printf("[INFO] /initmode user=%s arg=%q", user, arg)

	modeLabel := func(m int32) string {
		if m == 1 {
			return "bare (no state_init)"
		}
		return "full state_init"
	}
	if arg == "" {
		reply(ctx, b, chat, fmt.Sprintf("Current jetton init mode: %d (%s)\nUsage: /initmode <0|1>",
			r.jettonInitMode.Load(), modeLabel(r.jettonInitMode.Load())))
		return
	}
	if arg != "0" && arg != "1" {
		reply(ctx, b, chat, "init mode must be 0 (full state_init) or 1 (bare). Usage: /initmode <0|1>")
		return
	}
	mode := int32(0)
	if arg == "1" {
		mode = 1
	}
	r.jettonInitMode.Store(mode)
	r.logger.Printf("[INFO] jetton init mode set user=%s mode=%d", user, mode)
	r.audit("jetton init mode set to %d (%s)\nby: %s", mode, modeLabel(mode), user)
	reply(ctx, b, chat, fmt.Sprintf("Jetton init mode set to %d (%s)", mode, modeLabel(mode)))
}

// /errors [on|off|1|0] — toggle the error-breakdown tail appended to every
// status message. With no argument, reports current state.
func (r *Runtime) handleErrors(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	arg := strings.ToLower(argAt(msgText(update.Message), 1))
	r.logger.Printf("[INFO] /errors user=%s arg=%q", user, arg)

	if arg == "" {
		state := "off"
		if r.ErrorsEnabled() {
			state = "on"
		}
		reply(ctx, b, chat, fmt.Sprintf("Errors breakdown: %s\nUsage: /errors on|off", state))
		return
	}
	switch arg {
	case "on", "1", "true":
		r.SetErrorsEnabled(true)
		r.audit("errors breakdown: on\nby: %s", user)
		reply(ctx, b, chat, "Errors breakdown: on")
	case "off", "0", "false":
		r.SetErrorsEnabled(false)
		r.audit("errors breakdown: off\nby: %s", user)
		reply(ctx, b, chat, "Errors breakdown: off")
	default:
		reply(ctx, b, chat, "Usage: /errors on|off")
	}
}

// /retry [on|off|1|0] — toggle the pool's refetch+resend on a failed send. on (default) = retry once;
// off = single attempt (pre-"Retries" behavior). Takes effect live on running loops. No arg = report state.
func (r *Runtime) handleRetry(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	arg := strings.ToLower(argAt(msgText(update.Message), 1))
	r.logger.Printf("[INFO] /retry user=%s arg=%q", user, arg)

	state := func() string {
		if r.noRetry.Load() {
			return "off (single attempt)"
		}
		return "on (refetch + resend once)"
	}
	if arg == "" {
		reply(ctx, b, chat, fmt.Sprintf("Send retry: %s\nUsage: /retry on|off", state()))
		return
	}
	switch arg {
	case "on", "1", "true":
		r.noRetry.Store(false)
	case "off", "0", "false":
		r.noRetry.Store(true)
	default:
		reply(ctx, b, chat, "Usage: /retry on|off")
		return
	}
	r.audit("send retry: %s\nby: %s", state(), user)
	reply(ctx, b, chat, "Send retry: "+state())
}

// /check_balance [on|off] [threshold_nano] — gate jetton sends on the sender's on-chain GRAM balance.
// When on: a wallet below the threshold is skipped (counted Skipped, never Sent/Failed) and marked drained
// (subsequent picks skip it with no RPC); recipients are drawn from the live set; once 75% of the pool is
// drained the /jetton run auto-stops. Off by default. Takes effect live. No arg = report state.
func (r *Runtime) handleCheckBalance(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	arg := strings.ToLower(argAt(msgText(update.Message), 1))
	thrArg := argAt(msgText(update.Message), 2)
	r.logger.Printf("[INFO] /check_balance user=%s arg=%q thr=%q", user, arg, thrArg)

	report := func() string {
		st := "off"
		if r.checkBalance.Load() {
			st = "on"
		}
		nano := r.minSendBalance.Load()
		return fmt.Sprintf("Balance check: %s\nthreshold: %.4f GRAM (%d nano)\nauto-stop at %d%% drained\nUsage: /check_balance on|off [threshold_nano]",
			st, float64(nano)/1e9, nano, drain.StopPercent)
	}
	if arg == "" {
		reply(ctx, b, chat, report())
		return
	}
	switch arg {
	case "on", "1", "true":
		r.checkBalance.Store(true)
	case "off", "0", "false":
		r.checkBalance.Store(false)
	default:
		reply(ctx, b, chat, "Usage: /check_balance on|off [threshold_nano]")
		return
	}
	if thrArg != "" {
		n, err := strconv.ParseInt(thrArg, 10, 64)
		if err != nil || n <= 0 {
			reply(ctx, b, chat, "threshold must be a positive integer (nanoTON)")
			return
		}
		r.minSendBalance.Store(n)
	}
	r.audit("balance check: %s (threshold %d nano)\nby: %s", map[bool]string{true: "on", false: "off"}[r.checkBalance.Load()], r.minSendBalance.Load(), user)
	reply(ctx, b, chat, report())
}

// /api [url] [key]
func (r *Runtime) handleAPI(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	url := argAt(msgText(update.Message), 1)
	key := argAt(msgText(update.Message), 2)
	r.logger.Printf("[INFO] /api user=%s url=%q hasKey=%t", user, url, key != "")

	if url == "" {
		if r.defaultHTTP == nil {
			reply(ctx, b, chat, "No default HTTP endpoint (TONCENTER_ENDPOINT not set at startup)")
			return
		}
		r.SetBackend(r.defaultHTTP)
		r.audit("backend -> default http\nby: %s\nendpoint: %s", user, r.defaultHTTP.Description())
		reply(ctx, b, chat, "Reverted to default HTTP endpoint: "+r.defaultHTTP.Description())
		return
	}
	r.SetBackend(backend.NewHTTP(url, key))
	suffix := ""
	if key != "" {
		suffix = " (with key)"
	}
	r.audit("backend -> http\nby: %s\nurl: %s\nkey: %t", user, url, key != "")
	reply(ctx, b, chat, fmt.Sprintf("Switched to HTTP backend: %s%s", url, suffix))
}

// /config — interactive: ask user to upload JSON; the document handler takes over.
func (r *Runtime) handleConfig(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil || update.Message.From == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.pendingCfg.Store(update.Message.From.ID, struct{}{})
	r.logger.Printf("[INFO] /config requested user=%s", user)
	reply(ctx, b, chat, "Send the liteserver config .json file. Any non-document reply cancels.")
}

// document handler: if user is in pendingCfg, treat as LS config upload.
func (r *Runtime) handleDocument(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil || update.Message.From == nil {
		return
	}
	uid := update.Message.From.ID
	if _, waiting := r.pendingCfg.Load(uid); !waiting {
		return // not for us
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)

	if update.Message.Document == nil {
		r.pendingCfg.Delete(uid)
		reply(ctx, b, chat, "Config upload cancelled")
		return
	}
	r.pendingCfg.Delete(uid)
	doc := update.Message.Document
	r.logger.Printf("[INFO] config upload user=%s name=%q size=%d", user, doc.FileName, doc.FileSize)

	file, err := b.GetFile(ctx, &tg.GetFileParams{FileID: doc.FileID})
	if err != nil {
		reply(ctx, b, chat, fmt.Sprintf("Config load failed: GetFile %v", err))
		return
	}
	link := b.FileDownloadLink(file)
	resp, err := http.Get(link)
	if err != nil {
		reply(ctx, b, chat, fmt.Sprintf("Config load failed: download %v", err))
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		reply(ctx, b, chat, fmt.Sprintf("Config load failed: download status %d", resp.StatusCode))
		return
	}
	tmp, err := os.CreateTemp("", "ton-config-*.json")
	if err != nil {
		reply(ctx, b, chat, fmt.Sprintf("Config load failed: tmpfile %v", err))
		return
	}
	defer os.Remove(tmp.Name())
	if _, err := io.Copy(tmp, resp.Body); err != nil {
		tmp.Close()
		reply(ctx, b, chat, fmt.Sprintf("Config load failed: write %v", err))
		return
	}
	tmp.Close()

	be, count, err := backend.NewLSFromConfigFile(tmp.Name())
	if err != nil {
		reply(ctx, b, chat, fmt.Sprintf("Config load failed: %v", err))
		return
	}
	r.SetBackend(be)
	r.logger.Printf("[INFO] backend switch -> lite (%d servers) user=%s", count, user)
	r.audit("backend -> liteserver\nby: %s\nservers: %d\nfile: %s", user, count, doc.FileName)
	reply(ctx, b, chat, fmt.Sprintf("Switched to liteserver backend (%d servers)", count))
}

// /jetton <freq>
func (r *Runtime) handleJetton(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /jetton user=%s text=%q", user, msgText(update.Message))

	if active := r.poolRun.Load(); active != nil {
		reply(ctx, b, chat, fmt.Sprintf("Already running: %s. sent=%d, failed=%d", active.kind, active.stats.Sent.Load(), active.stats.Failed.Load()))
		return
	}
	freqArg := argAt(msgText(update.Message), 1)
	if freqArg == "" {
		reply(ctx, b, chat, "Usage: /jetton <freq>")
		return
	}
	freq, ok := parsePositiveInt(freqArg)
	if !ok {
		reply(ctx, b, chat, "freq must be a positive integer. Usage: /jetton <freq>")
		return
	}
	periodMs := time.Duration(1000/freq) * time.Millisecond

	// One randomly-chosen sub-pool per run, so the 2.5M pool is spent evenly across runs.
	poolStart, poolCount := r.pickSubPool()
	jwalletCode := r.jwCode

	p := r.initSpamPool()
	replyHTML(ctx, b, chat, fmt.Sprintf("Starting jetton spam\nWallets: %s\nFreq: %d req/s (%dms)",
		r.poolWindowLine(p, poolStart, poolCount), freq, periodMs/time.Millisecond))
	stats := &pool.Stats{}
	errs := NewErrStack()
	loopCtx, cancel := context.WithCancel(context.Background())
	run := &SpamRun{cancel: cancel, stats: stats, freq: freq, period: periodMs, startedAt: time.Now(), kind: "jetton", errs: errs}
	if !r.poolRun.CompareAndSwap(nil, run) {
		cancel()
		reply(ctx, b, chat, "Already running")
		return
	}

	// /check_balance drain tracking for THIS run. The Gate, recipient filtering, and 75% auto-stop only
	// activate while r.checkBalance is on (read live), so the tracker is always built but stays dormant
	// when the toggle is off — flipping /check_balance mid-run takes effect immediately.
	var tracker *drain.Tracker
	// The tracker indexes RELATIVE to the sub-pool (id-poolStart), so "75% drained" means 75% of the
	// wallets this run actually drives, not of the whole 2.5M pool.
	tracker = drain.New(poolCount, func() {
		n := tracker.Count()
		r.logger.Printf("[INFO] jetton auto-stop: %d/%d wallets drained (>=%d%%)", n, poolCount, drain.StopPercent)
		reply(loopCtx, b, chat, fmt.Sprintf("⏹ Auto-stopped: %d%% of the sub-pool is drained (%d/%d). Top up the wallets to resume.", drain.StopPercent, n, poolCount))
		r.audit("jetton auto-stopped: %d%%+ of sub-pool drained (%d/%d)", drain.StopPercent, n, poolCount)
		cancel()
	})

	cfg := pool.LoopConfig{
		Rate:        float64(freq),
		MaxInflight: int64(freq*2 + 10),
		SendTimeout: r.SendTimeout,
		ErrSink:     errs.Record,
		StatsEvery:  uint64(freq) * 30,
		NoRetry:     r.noRetry.Load,
		WindowStart: poolStart,
		WindowCount: poolCount,
		// Gate on wallet balance when /check_balance is on: skip drained wallets (Skipped, never
		// Sent/Failed), fail-closed on a balance RPC error, and mark+auto-stop at 75% drained.
		Gate: func(ctx context.Context, e *pool.Entry) bool {
			if !r.checkBalance.Load() {
				return true
			}
			id := int(e.Wallet.SubwalletID) - poolStart
			if tracker.IsDrained(id) {
				return false // known-drained: skip with no RPC
			}
			bal, err := r.Backend().GetBalance(ctx, e.Wallet.Address())
			if err != nil {
				errs.Record("checkBalance", err.Error())
				return false // fail-closed: only send when balance is confirmed
			}
			if bal.Cmp(big.NewInt(r.minSendBalance.Load())) < 0 {
				tracker.Mark(id) // may trigger the 75% auto-stop
				return false
			}
			return true
		},
		Send: func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			var chosen *address.Address
			if v := observedFrom(ctx); v != nil {
				chosen = v.to
			}
			body, err := r.buildJettonExternalForEntry(jwalletCode, tracker, e, seqno, poolStart, poolCount, chosen)
			if err != nil {
				return err
			}
			r.bindObserved(ctx, e.Wallet.Address(), body)
			if err := r.Backend().SendExternalMessage(ctx, e.Wallet.Address(), body); err != nil {
				return err
			}
			return nil
		},
		FetchSeqno: func(ctx context.Context, e *pool.Entry) (uint32, error) {
			return r.Backend().GetSeqno(ctx, e.Wallet.Address())
		},
	}
	r.watchTransfers(&cfg, run, func(*pool.Entry) *address.Address {
		id := mathrand.Intn(poolCount)
		if r.checkBalance.Load() {
			id = tracker.RandomLive()
		}
		return p.Get(poolStart + id).Wallet.Address()
	})
	tickStart := time.Now()
	var lastSnap Snap
	cfg.OnTick = func(s *pool.Stats) {
		periodSec := time.Since(tickStart).Seconds()
		topErrs, totalErrs := errs.SnapshotDelta(10)
		WriteSnapshot(r.errLog, "jetton", topErrs, totalErrs, periodSec)
		text := formatStatusHTML("jetton", s, lastSnap, periodSec)
		text += r.streamLine(run)
		if r.checkBalance.Load() {
			n := tracker.Count()
			text += fmt.Sprintf("\n<code>drained %d/%d (%.0f%%) · auto-stop @ %d%%</code>",
				n, poolCount, 100*float64(n)/float64(poolCount), drain.StopPercent)
		}
		if r.ErrorsEnabled() {
			text += FormatBreakdownHTML(topErrs, totalErrs, periodSec)
		}
		r.sendStatus(loopCtx, b, chat, text)
		r.auditEdit(run.auditMsgID, r.auditTransferStatus(run, s, time.Since(run.startedAt).Seconds()))
		tickStart = time.Now()
		lastSnap = takeSnap(s)
	}

	run.auditHeader = fmt.Sprintf("start jetton spam\nby: %s\nfreq: %d req/s\ninit mode: %d\nwallets: %d %s\nbackend: %s",
		user, freq, r.jettonInitMode.Load(), poolCount, poolRangeText(poolStart, poolCount, r.poolTotal),
		r.Backend().Description())
	run.auditMsgID = r.auditMessage(r.auditTransferStatus(run, stats, 0))

	go func() {
		defer r.poolRun.CompareAndSwap(run, nil)
		p.Run(loopCtx, cfg, stats)
		r.logger.Printf("[INFO] jetton loop finished sent=%d failed=%d", stats.Sent.Load(), stats.Failed.Load())
	}()
}

// /jetton_stop
func (r *Runtime) handleJettonStop(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /jetton_stop user=%s", user)
	run := r.poolRun.Load()
	if run == nil || run.kind != "jetton" {
		reply(ctx, b, chat, "Spam is not running")
		return
	}
	run.cancel()
	elapsed := time.Since(run.startedAt).Seconds()
	sent := run.stats.Sent.Load()
	failed := run.stats.Failed.Load()
	avg := 0.0
	if elapsed > 0 {
		avg = float64(sent) / elapsed
	}
	r.audit("stop jetton spam\nby: %s\nsent %d, failed %d, %.0f/s, %.0fs",
		user, sent, failed, avg, elapsed)
	reply(ctx, b, chat, fmt.Sprintf(
		"Stopping jetton spam...\nSent: %d, Failed: %d\nElapsed: %.1fs, Avg: %.1f sent/s",
		sent, failed, elapsed, avg))
}

// /external <freq>
func (r *Runtime) handleExternal(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /external user=%s text=%q", user, msgText(update.Message))

	if active := r.poolRun.Load(); active != nil {
		reply(ctx, b, chat, fmt.Sprintf("Already running: %s. sent=%d, failed=%d", active.kind, active.stats.Sent.Load(), active.stats.Failed.Load()))
		return
	}
	freqArg := argAt(msgText(update.Message), 1)
	if freqArg == "" {
		reply(ctx, b, chat, "Usage: /external <freq>")
		return
	}
	freq, ok := parsePositiveInt(freqArg)
	if !ok {
		reply(ctx, b, chat, "freq must be a positive integer. Usage: /external <freq>")
		return
	}
	periodMs := time.Duration(1000/freq) * time.Millisecond
	poolStart, poolCount := r.pickSubPool()
	p := r.initSpamPool()
	replyHTML(ctx, b, chat, fmt.Sprintf("Starting external spam\nWallets: %s\nFreq: %d req/s (%dms)",
		r.poolWindowLine(p, poolStart, poolCount), freq, periodMs/time.Millisecond))
	stats := &pool.Stats{}
	errs := NewErrStack()
	loopCtx, cancel := context.WithCancel(context.Background())
	run := &SpamRun{cancel: cancel, stats: stats, freq: freq, period: periodMs, startedAt: time.Now(), kind: "external", errs: errs}
	if !r.poolRun.CompareAndSwap(nil, run) {
		cancel()
		reply(ctx, b, chat, "Already running")
		return
	}

	cfg := pool.LoopConfig{
		Rate:        float64(freq),
		MaxInflight: int64(freq*2 + 10),
		SendTimeout: r.SendTimeout,
		ErrSink:     errs.Record,
		StatsEvery:  uint64(freq) * 30,
		NoRetry:     r.noRetry.Load,
		WindowStart: poolStart,
		WindowCount: poolCount,
		Send: func(ctx context.Context, e *pool.Entry, seqno uint32) error {
			body := e.Wallet.BuildExternalBody(seqno, uint32(time.Now().Unix()+40))
			return r.Backend().SendExternalMessage(ctx, e.Wallet.Address(), body)
		},
		FetchSeqno: func(ctx context.Context, e *pool.Entry) (uint32, error) {
			return r.Backend().GetSeqno(ctx, e.Wallet.Address())
		},
	}
	tickStart := time.Now()
	var lastSnap Snap
	cfg.OnTick = func(s *pool.Stats) {
		periodSec := time.Since(tickStart).Seconds()
		topErrs, totalErrs := errs.SnapshotDelta(10)
		WriteSnapshot(r.errLog, "external", topErrs, totalErrs, periodSec)
		text := formatStatusHTML("external", s, lastSnap, periodSec)
		if r.ErrorsEnabled() {
			text += FormatBreakdownHTML(topErrs, totalErrs, periodSec)
		}
		r.sendStatus(loopCtx, b, chat, text)
		r.auditEdit(run.auditMsgID, run.auditHeader+"\n"+auditStatLine(s, time.Since(run.startedAt).Seconds()))
		tickStart = time.Now()
		lastSnap = takeSnap(s)
	}

	run.auditHeader = fmt.Sprintf("start external spam\nby: %s\nfreq: %d req/s\nwallets: %d %s\nbackend: %s",
		user, freq, poolCount, poolRangeText(poolStart, poolCount, r.poolTotal),
		r.Backend().Description())
	run.auditMsgID = r.auditMessage(run.auditHeader + "\n" + auditStatLine(stats, 0))

	go func() {
		defer r.poolRun.CompareAndSwap(run, nil)
		p.Run(loopCtx, cfg, stats)
		r.logger.Printf("[INFO] external loop finished sent=%d failed=%d", stats.Sent.Load(), stats.Failed.Load())
	}()
}

// /external_stop
func (r *Runtime) handleExternalStop(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /external_stop user=%s", user)
	run := r.poolRun.Load()
	if run == nil || run.kind != "external" {
		reply(ctx, b, chat, "External spam is not running")
		return
	}
	run.cancel()
	elapsed := time.Since(run.startedAt).Seconds()
	sent := run.stats.Sent.Load()
	failed := run.stats.Failed.Load()
	avg := 0.0
	if elapsed > 0 {
		avg = float64(sent) / elapsed
	}
	r.audit("stop external spam\nby: %s\nsent %d, failed %d, %.0f/s, %.0fs",
		user, sent, failed, avg, elapsed)
	reply(ctx, b, chat, fmt.Sprintf(
		"Stopping external spam...\nSent: %d, Failed: %d\nElapsed: %.1fs, Avg: %.1f sent/s",
		sent, failed, elapsed, avg))
}
