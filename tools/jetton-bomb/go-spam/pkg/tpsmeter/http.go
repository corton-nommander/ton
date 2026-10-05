package tpsmeter

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strconv"
	"time"
)

// NewHTTPHandler returns the JSON API for a persistent meter and store.
func NewHTTPHandler(meter *Meter, store *Store) http.Handler {
	return &httpHandler{meter: meter, store: store}
}

type httpHandler struct {
	meter *Meter
	store *Store
}

func (h *httpHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "method not allowed"})
		return
	}
	switch r.URL.Path {
	case "/v1/health":
		h.health(w)
	case "/v1/status":
		h.status(w)
	case "/v1/head":
		h.head(w, r)
	case "/v1/series":
		h.series(w, r)
	case "/v1/summary":
		h.summary(w, r)
	default:
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
	}
}

func (h *httpHandler) head(w http.ResponseWriter, r *http.Request) {
	head, err := h.meter.LiveHead(r.Context())
	if err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, head)
}

func (h *httpHandler) health(w http.ResponseWriter) {
	status, err := h.store.Status()
	if err != nil {
		h.meter.recordErr(err)
		writeJSON(w, http.StatusServiceUnavailable, map[string]any{"ok": false, "error": err.Error()})
		return
	}
	snapshot := h.meter.Snapshot()
	ok := status.CoverageValid && status.CoverageStart != nil && status.CoverageEnd != nil &&
		status.PartialBlocks == 0 && snapshot.CoverageValid && snapshot.BackfillComplete &&
		snapshot.SourceVerified && snapshot.ForwardSynced && snapshot.WindowsReady && !snapshot.PendingBatch &&
		snapshot.SkippedMC == 0 && snapshot.SkippedShards == 0 && !snapshot.RollbackDetected
	code := http.StatusOK
	if !ok {
		code = http.StatusServiceUnavailable
	}
	writeJSON(w, code, map[string]any{
		"ok":                ok,
		"epoch":             status.Epoch,
		"blocks":            status.Blocks,
		"coverage_valid":    status.CoverageValid,
		"coverage_start":    status.CoverageStart,
		"coverage_end":      status.CoverageEnd,
		"partial_blocks":    status.PartialBlocks,
		"rollback_detected": status.RollbackDetected,
		"pending_batch":     snapshot.PendingBatch,
		"source_verified":   snapshot.SourceVerified,
		"forward_synced":    snapshot.ForwardSynced,
		"windows_ready":     snapshot.WindowsReady,
		"backfill_complete": snapshot.BackfillComplete,
	})
}

func (h *httpHandler) status(w http.ResponseWriter) {
	storeStatus, err := h.store.Status()
	if err != nil {
		h.meter.recordErr(err)
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	snapshot := h.meter.Snapshot()
	writeJSON(w, http.StatusOK, struct {
		Source              string      `json:"source"`
		UptimeSeconds       float64     `json:"uptime_seconds"`
		Shards              int         `json:"shards"`
		Windows             []WindowTPS `json:"windows"`
		Polls               int         `json:"polls"`
		Errors              int         `json:"errors"`
		LastError           string      `json:"last_error,omitempty"`
		MCCatchupSkips      int         `json:"mc_catchup_skips"`
		ShardCatchupSkips   int         `json:"shard_catchup_skips"`
		PartialBlocks       uint64      `json:"partial_blocks"`
		CoverageValid       bool        `json:"coverage_valid"`
		RollbackDetected    bool        `json:"rollback_detected"`
		LastMismatchAt      *time.Time  `json:"last_mismatch_at,omitempty"`
		PendingBatch        bool        `json:"pending_batch"`
		SourceVerified      bool        `json:"source_verified"`
		ForwardSynced       bool        `json:"forward_synced"`
		WindowsReady        bool        `json:"windows_ready"`
		BackfillActive      bool        `json:"backfill_active"`
		BackfillComplete    bool        `json:"backfill_complete"`
		BackfillOldestUtime int64       `json:"backfill_oldest_utime,omitempty"`
		BackfillTargetUtime int64       `json:"backfill_target_utime,omitempty"`
		Store               StoreStatus `json:"store"`
	}{
		Source:              h.meter.Source().Description(),
		UptimeSeconds:       h.meter.Uptime().Seconds(),
		Shards:              snapshot.Shards,
		Windows:             snapshot.Windows,
		Polls:               snapshot.Polls,
		Errors:              snapshot.Errors,
		LastError:           snapshot.LastErr,
		MCCatchupSkips:      snapshot.SkippedMC,
		ShardCatchupSkips:   snapshot.SkippedShards,
		PartialBlocks:       storeStatus.PartialBlocks,
		CoverageValid:       storeStatus.CoverageValid,
		RollbackDetected:    snapshot.RollbackDetected,
		LastMismatchAt:      storeStatus.LastMismatchAt,
		PendingBatch:        snapshot.PendingBatch,
		SourceVerified:      snapshot.SourceVerified,
		ForwardSynced:       snapshot.ForwardSynced,
		WindowsReady:        snapshot.WindowsReady,
		BackfillActive:      snapshot.BackfillActive,
		BackfillComplete:    snapshot.BackfillComplete,
		BackfillOldestUtime: snapshot.BackfillOldestUtime,
		BackfillTargetUtime: snapshot.BackfillTargetUtime,
		Store:               storeStatus,
	})
}

func (h *httpHandler) series(w http.ResponseWriter, r *http.Request) {
	start, end, window, err := parseRangeQuery(r)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	step := 0
	if values, ok := r.URL.Query()["step"]; ok {
		if len(values) != 1 || values[0] == "" {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "step must be one positive integer"})
			return
		}
		step, err = strconv.Atoi(values[0])
		if err != nil || step <= 0 {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "step must be a positive integer"})
			return
		}
	}
	result, err := h.store.Series(start, end, window, step)
	if err != nil {
		h.meter.recordErr(err)
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	applyRuntimeQuality(h.meter.Snapshot(), result.IncludesActiveEpoch, &result.Complete)
	writeJSON(w, http.StatusOK, result)
}

func (h *httpHandler) summary(w http.ResponseWriter, r *http.Request) {
	start, end, window, err := parseRangeQuery(r)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	result, err := h.store.Summary(start, end, window)
	if err != nil {
		h.meter.recordErr(err)
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	applyRuntimeQuality(h.meter.Snapshot(), result.IncludesActiveEpoch, &result.Complete)
	writeJSON(w, http.StatusOK, result)
}

func parseRangeQuery(r *http.Request) (time.Time, time.Time, int, error) {
	query := r.URL.Query()
	readOne := func(name string) (string, error) {
		values := query[name]
		if len(values) != 1 || values[0] == "" {
			return "", fmt.Errorf("%s is required exactly once", name)
		}
		return values[0], nil
	}
	startRaw, err := readOne("start")
	if err != nil {
		return time.Time{}, time.Time{}, 0, err
	}
	endRaw, err := readOne("end")
	if err != nil {
		return time.Time{}, time.Time{}, 0, err
	}
	windowRaw, err := readOne("window_size")
	if err != nil {
		return time.Time{}, time.Time{}, 0, err
	}
	start, err := time.Parse(time.RFC3339, startRaw)
	if err != nil {
		return time.Time{}, time.Time{}, 0, fmt.Errorf("invalid start: %w", err)
	}
	end, err := time.Parse(time.RFC3339, endRaw)
	if err != nil {
		return time.Time{}, time.Time{}, 0, fmt.Errorf("invalid end: %w", err)
	}
	// Browser date ranges use ISO timestamps with milliseconds. The store is
	// second-granular, so normalize HTTP boundaries without weakening its exact
	// internal query contract.
	start = start.UTC().Truncate(time.Second)
	end = end.UTC().Truncate(time.Second)
	window, err := strconv.Atoi(windowRaw)
	if err != nil || window <= 0 {
		return time.Time{}, time.Time{}, 0, fmt.Errorf("window_size must be a positive integer")
	}
	if _, _, err := validateRange(start, end, window); err != nil {
		return time.Time{}, time.Time{}, 0, err
	}
	return start, end, window, nil
}

func applyRuntimeQuality(snapshot Snapshot, includesActiveEpoch bool, complete *bool) {
	if !includesActiveEpoch {
		return
	}
	if !snapshot.CoverageValid || !snapshot.SourceVerified || snapshot.SkippedMC != 0 || snapshot.SkippedShards != 0 || snapshot.RollbackDetected {
		*complete = false
	}
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
