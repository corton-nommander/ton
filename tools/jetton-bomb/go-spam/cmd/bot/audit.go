package main

import (
	"context"
	"fmt"
	"time"

	tg "github.com/go-telegram/bot"

	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
)

// audit posts a single one-off event to the private audit chat configured via AUDIT_CHAT_ID.
// No-op when the audit log is disabled (auditChatID == 0). Best-effort — failures are logged and
// swallowed, never surfaced to the command handler. Plain text (no ParseMode) so arbitrary
// usernames/URLs can't break formatting or inject markup.
//
// AUDIT_CHAT_ID may be a channel (-100…), a group (-…), or a private chat (positive user id) — the
// send is the same. Decoupled from the per-run status ticks (sendStatus): the audit chat gets
// who-did-what and the live run line, NOT the periodic rich TPS status.
func (r *Runtime) audit(format string, args ...any) {
	r.auditMessage(fmt.Sprintf(format, args...))
}

// auditMessage posts text and returns the sent message id (0 when disabled or on failure). The id is
// used to keep a run's message updating in place (auditEdit) instead of spamming the chat every tick.
func (r *Runtime) auditMessage(text string) int {
	if r.auditChatID == 0 || r.bot == nil {
		return 0
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	m, err := r.bot.SendMessage(ctx, &tg.SendMessageParams{ChatID: r.auditChatID, Text: text})
	if err != nil {
		r.logger.Printf("[WARN] audit send to chat %d failed: %v", r.auditChatID, err)
		return 0
	}
	return m.ID
}

// auditEdit replaces the text of a previously-sent audit message (the live run stat line), so the run's
// message updates in place rather than emitting a new message each tick. No-op when disabled or
// msgID == 0. Best-effort: a failed edit (e.g. Telegram "message is not modified") is logged only.
func (r *Runtime) auditEdit(msgID int, text string) {
	if r.auditChatID == 0 || r.bot == nil || msgID == 0 {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if _, err := r.bot.EditMessageText(ctx, &tg.EditMessageTextParams{ChatID: r.auditChatID, MessageID: msgID, Text: text}); err != nil {
		r.logger.Printf("[WARN] audit edit chat %d msg %d failed: %v", r.auditChatID, msgID, err)
	}
}

// auditStatLine is the compact one-line run summary appended to a run's live audit message and refreshed
// on every status tick (in sync with sendStatus). Cumulative totals + overall send rate + elapsed.
func auditStatLine(s *pool.Stats, elapsed float64) string {
	sent := s.Sent.Load()
	failed := s.Failed.Load()
	avg := 0.0
	if elapsed > 0 {
		avg = float64(sent) / elapsed
	}
	return fmt.Sprintf("sent %d, failed %d, %.0f/s, %.0fs", sent, failed, avg, elapsed)
}
