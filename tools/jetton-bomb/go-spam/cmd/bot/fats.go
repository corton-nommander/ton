package main

// /fat_gram, /fat_jetton, /fat_size — pool spam aimed at the FAT load-target accounts that inflate the
// controlled testnet's state (10/25/50/100/200 KB per account, 781k accounts, ~160 GB total).
//
// The point of these modes is the COLLATOR, not the transfer: touching an account forces the collator to
// walk its storage dict (prelim_storage_stat), which is the long_collation split axis. Bigger fats ⇒ more
// walking per message, so /fat_size selects the tier.
//
// The recipients are derived, never stored — see pkg/fatpool. Sender is a pool WalletSpam signing its own
// external (same path as /gram and /jetton, not the highload-relayed gasless one).

import (
	"context"
	"fmt"
	"strconv"
	"strings"
	"time"

	tg "github.com/go-telegram/bot"
	"github.com/go-telegram/bot/models"

	"github.com/xssnick/tonutils-go/address"

	"github.com/victor/jetton-bomb-go-spam/pkg/fatpool"
	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
)

const (
	modeFatGram   = "fat_gram"
	modeFatJetton = "fat_jetton"

	// DefaultFatSizeKB is the tier /fat_size starts on: the 100 KB accounts, the largest tier with a big
	// population (431k of them).
	DefaultFatSizeKB = 100
)

func (r *Runtime) handleFatGram(ctx context.Context, b *tg.Bot, update *models.Update) {
	r.handleTransferSpam(ctx, b, update, modeFatGram)
}

func (r *Runtime) handleFatJetton(ctx context.Context, b *tg.Bot, update *models.Update) {
	r.handleTransferSpam(ctx, b, update, modeFatJetton)
}

func (r *Runtime) handleFatGramStop(ctx context.Context, b *tg.Bot, update *models.Update) {
	r.handleTransferSpamStop(ctx, b, update, modeFatGram)
}

func (r *Runtime) handleFatJettonStop(ctx context.Context, b *tg.Bot, update *models.Update) {
	r.handleTransferSpamStop(ctx, b, update, modeFatJetton)
}

// fatTargets returns (deriving once, then cached) every fat address of the given size tier. Derivation is
// CPU-bound — ~1s for the 431k-account 100 KB tier — so it happens here, before a run starts, rather than
// on the send path.
func (r *Runtime) fatTargets(sizeKB int) ([]*address.Address, error) {
	r.fatMu.Lock()
	defer r.fatMu.Unlock()
	if addrs, ok := r.fatAddrs[sizeKB]; ok {
		return addrs, nil
	}
	started := time.Now()
	addrs, err := fatpool.Derive(sizeKB)
	if err != nil {
		return nil, err
	}
	if r.fatAddrs == nil {
		r.fatAddrs = map[int][]*address.Address{}
	}
	r.fatAddrs[sizeKB] = addrs
	r.logger.Printf("[INFO] derived %d fat addresses of %dKB in %s", len(addrs), sizeKB, time.Since(started).Round(time.Millisecond))
	return addrs, nil
}

// /fat_size <10|25|50|100|200> — pick the fat tier /fat_gram and /fat_jetton aim at.
func (r *Runtime) handleFatSize(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	r.logger.Printf("[INFO] /fat_size user=%s text=%q", user, msgText(update.Message))

	arg := argAt(msgText(update.Message), 1)
	if arg == "" {
		var sb string
		for _, s := range fatpool.Sizes() {
			mark := ""
			if s == int(r.fatSize.Load()) {
				mark = " <- current"
			}
			sb += fmt.Sprintf("%d KB — %d accounts%s\n", s, fatpool.CountFor(s), mark)
		}
		reply(ctx, b, chat, "Fat target size. Usage: /fat_size <10|25|50|100|200>\n\n"+sb)
		return
	}
	size, ok := parsePositiveInt(arg)
	if !ok || !fatpool.HasSize(size) {
		reply(ctx, b, chat, fmt.Sprintf("Unknown size %q. Deployed tiers: %v KB", arg, fatpool.Sizes()))
		return
	}
	prev := r.fatSize.Swap(int32(size))
	msg := fmt.Sprintf("Fat target size: %d KB (%d accounts)", size, fatpool.CountFor(size))
	// A target count is only meaningful against a tier — carrying it across a tier change would silently
	// mean something different (or get clamped), so switching tiers resets it to "all of them".
	if int(prev) != size && r.fatCount.Swap(0) != 0 {
		msg += "\nTarget count reset to all accounts of the new tier (/fat_count)"
	}
	// A running loop captured its recipient list at start, so the new tier only applies to the NEXT run.
	if run := r.poolRun.Load(); run != nil && (run.kind == modeFatGram || run.kind == modeFatJetton) {
		msg += fmt.Sprintf("\n\n⚠️ %s is running on the previous tier — restart it to use %d KB.", run.kind, size)
	}
	r.audit("fat size -> %d KB\nby: %s", size, user)
	reply(ctx, b, chat, msg)
}

// /fat_count <n> — how many fat accounts of the current tier one run aims at.
//
// Default (and what /fat_size resets it to) is the whole tier. Below that, the tier is cut into fixed
// slices of n and each run draws one — the same scheme /pool_size applies to the senders, so the fats
// wear evenly across runs instead of a hot subset being hit every time.
//
// Takes effect on the NEXT run: a running loop already captured its recipient list.
func (r *Runtime) handleFatCount(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	arg := argAt(msgText(update.Message), 1)
	r.logger.Printf("[INFO] /fat_count user=%s arg=%q", user, arg)

	size := int(r.fatSize.Load())
	total := fatpool.CountFor(size)
	describe := func() string {
		n := r.FatCount(total)
		if n >= total {
			return fmt.Sprintf("Fat targets: all %s accounts of %d KB", groupDigits(total), size)
		}
		return fmt.Sprintf("Fat targets: %s of %s accounts of %d KB",
			groupDigits(n), groupDigits(total), size)
	}
	usage := fmt.Sprintf("Usage: /fat_count <n> — fat accounts aimed at per run (1…%s)\nReset to all by /fat_size.",
		groupDigits(total))
	if arg == "" {
		reply(ctx, b, chat, describe()+"\n\n"+usage)
		return
	}
	n, ok := parsePositiveInt(arg)
	if !ok {
		reply(ctx, b, chat, "Count must be a positive integer.\n"+usage)
		return
	}
	clamped := ""
	if n > total {
		clamped = fmt.Sprintf("\n\n(%s is more than the %d KB tier holds — using all %s)",
			groupDigits(n), size, groupDigits(total))
	}
	r.fatCount.Store(int64(n))

	msg := describe() + clamped
	if run := r.poolRun.Load(); run != nil && (run.kind == modeFatGram || run.kind == modeFatJetton) {
		msg += fmt.Sprintf("\n\n⚠️ %s is running on the previously picked targets — restart it to apply.", run.kind)
	}
	r.audit("fat count -> %d targets of %d KB\nby: %s", r.FatCount(total), size, user)
	reply(ctx, b, chat, msg)
}

// /fat_mutate on|off — whether a /fat_jetton transfer sets the fat's mutate flag (touch AND rewrite its
// storage) or leaves it clear (touch and read only). That is the A/B knob for separating "the collator
// paid to WALK the big state" from "the collator also paid to REBUILD it".
//
// Read per send, so it takes effect mid-run — unlike /fat_size, which the running loop captured at start.
// Only /fat_jetton carries the flag: a plain /fat_gram transfer has no forward payload to put it in.
func (r *Runtime) handleFatMutate(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	arg := strings.ToLower(argAt(msgText(update.Message), 1))
	r.logger.Printf("[INFO] /fat_mutate user=%s arg=%q", user, arg)

	state := func() string {
		if r.fatMutate.Load() {
			return "on (touch + rewrite the fat's storage)"
		}
		return "off (touch + read only)"
	}
	if arg == "" {
		reply(ctx, b, chat, fmt.Sprintf("Fat mutate: %s\nUsage: /fat_mutate on|off\n\nApplies to /fat_jetton (the flag rides in the transfer's forward payload); /fat_gram has no payload to carry it.", state()))
		return
	}
	switch arg {
	case "on", "1", "true":
		r.fatMutate.Store(true)
	case "off", "0", "false":
		r.fatMutate.Store(false)
	default:
		reply(ctx, b, chat, "Usage: /fat_mutate on|off")
		return
	}
	msg := "Fat mutate: " + state()
	if run := r.poolRun.Load(); run != nil && run.kind == modeFatJetton {
		msg += "\n\nApplied live — the running /fat_jetton picks it up on the next send."
	}
	r.audit("fat mutate: %s\nby: %s", state(), user)
	reply(ctx, b, chat, msg)
}

// poolRangeText is the plain-text range for logs and the audit chat, where HTML links would be noise.
func poolRangeText(start, count, total int) string {
	if count >= total {
		return fmt.Sprintf("(whole pool of %s)", shortCount(total))
	}
	return fmt.Sprintf("[%s, %s) of %s", shortCount(start), shortCount(start+count), shortCount(total))
}

// shortCount renders a wallet count the way it is actually spoken: 100000 -> "100K", 2500000 -> "2.5M",
// 2100000 -> "2.1M". Falls back to grouped digits for values that are not round, so nothing is ever
// rounded away silently.
func shortCount(n int) string {
	switch {
	case n >= 1_000_000 && n%1_000_000 == 0:
		return fmt.Sprintf("%dM", n/1_000_000)
	case n >= 1_000_000 && n%100_000 == 0:
		return fmt.Sprintf("%.1fM", float64(n)/1e6)
	case n >= 1_000_000 && n%10_000 == 0:
		return fmt.Sprintf("%.2fM", float64(n)/1e6)
	case n >= 1_000 && n%1_000 == 0:
		return fmt.Sprintf("%dK", n/1_000)
	case n >= 10_000 && n%100 == 0:
		return fmt.Sprintf("%.1fK", float64(n)/1e3)
	default:
		return groupDigits(n)
	}
}

// testnetExplorer reports the tonviewer base for wallet links, or "" when links must be suppressed.
// Only testnet gets links: mainnet drives the whole (small) pool, so there is no window to point at, and
// a testnet URL there would be plain wrong. Detected from the backend description — an LS backend carries
// no network marker, so it simply gets no links rather than a guessed one.
func (r *Runtime) testnetExplorer() string {
	be := r.Backend()
	if be == nil {
		return ""
	}
	if strings.Contains(strings.ToLower(be.Description()), "testnet") {
		return "https://testnet.tonviewer.com/"
	}
	return ""
}

// poolWindowLine describes the wallet range a run drives, with the two boundaries linked to the actual
// wallets so the range can be opened in the explorer instead of hand-derived. HTML — send with replyHTML.
//
// The interval is half-open, and the links follow the notation literally: the left number links to the
// first wallet of the run, the right one to the first wallet AFTER it (unlinked when that is past the end
// of the deployed pool, since no such wallet exists).
func (r *Runtime) poolWindowLine(p *pool.Pool, start, count int) string {
	if count >= r.poolTotal {
		return fmt.Sprintf("%s (whole pool)", shortCount(r.poolTotal))
	}
	end := start + count
	base := r.testnetExplorer()
	lo, hi := shortCount(start), shortCount(end)
	if base != "" {
		lo = fmt.Sprintf("<a href=\"%s%s\">%s</a>", base, p.Get(start).Wallet.Address().String(), lo)
		if end < r.poolTotal {
			hi = fmt.Sprintf("<a href=\"%s%s\">%s</a>", base, p.Get(end).Wallet.Address().String(), hi)
		}
	}
	return fmt.Sprintf("%s [%s, %s) of %s", shortCount(count), lo, hi, shortCount(r.poolTotal))
}

// /pool_size <n> — how many wallets one run drives.
//
// The deployed pool (POOL_TOTAL) is cut into floor(total/n) fixed sub-pools and every run picks one at
// random, so the pool wears evenly across runs. n is CLAMPED to the deployed total, so a small pool (a 5k
// mainnet deploy, say) just runs whole-pool instead of erroring.
//
// Takes effect on the NEXT run: a running loop already picked its window.
func (r *Runtime) handlePoolSize(ctx context.Context, b *tg.Bot, update *models.Update) {
	if update.Message == nil {
		return
	}
	chat := update.Message.Chat.ID
	user := userTag(update.Message.From)
	arg := argAt(msgText(update.Message), 1)
	r.logger.Printf("[INFO] /pool_size user=%s arg=%q", user, arg)

	describe := func() string {
		n := r.SubPoolSize()
		slices := r.poolTotal / n
		if slices <= 1 {
			return fmt.Sprintf("%s wallets per run — the whole pool, no splitting",
				groupDigits(n))
		}
		s := fmt.Sprintf("%s wallets per run\nPool of %s is split into %d sub-pools of %s; each run drives one, picked at random",
			groupDigits(n), groupDigits(r.poolTotal), slices, groupDigits(n))
		if rem := r.poolTotal % n; rem != 0 {
			s += fmt.Sprintf("\nLast %s wallets are never used (pool doesn't divide evenly)", groupDigits(rem))
		}
		return s
	}
	if arg == "" {
		reply(ctx, b, chat, fmt.Sprintf("%s\n\nUsage: /pool_size <n> — how many wallets one run drives (1…%s)",
			describe(), groupDigits(r.poolTotal)))
		return
	}
	n, ok := parsePositiveInt(arg)
	if !ok {
		reply(ctx, b, chat, fmt.Sprintf("Size must be a positive integer.\nUsage: /pool_size <n> — how many wallets one run drives (1…%s)",
			groupDigits(r.poolTotal)))
		return
	}
	clamped := ""
	if n > r.poolTotal {
		clamped = fmt.Sprintf("\n\n(%s is bigger than the deployed pool — using all %s)",
			groupDigits(n), groupDigits(r.poolTotal))
	}
	r.subPoolSize.Store(int64(n))

	msg := describe() + clamped
	if run := r.poolRun.Load(); run != nil {
		msg += fmt.Sprintf("\n\n⚠️ %s is running on the previously picked sub-pool — restart it to apply.", run.kind)
	}
	r.audit("pool size -> %d wallets per run\nby: %s", r.SubPoolSize(), user)
	reply(ctx, b, chat, msg)
}

// groupDigits renders 2500000 as "2 500 000". Pool sizes are read at a glance in chat; unseparated
// seven-digit numbers are where "100000 per run out of 2500000" turns into a misread.
func groupDigits(n int) string {
	s := strconv.Itoa(n)
	if n < 0 {
		s = s[1:]
	}
	var out []byte
	for i, c := range []byte(s) {
		if i > 0 && (len(s)-i)%3 == 0 {
			out = append(out, ' ')
		}
		out = append(out, c)
	}
	if n < 0 {
		return "-" + string(out)
	}
	return string(out)
}
