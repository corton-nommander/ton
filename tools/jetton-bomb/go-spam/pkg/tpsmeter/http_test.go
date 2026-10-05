package tpsmeter

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"testing"
	"time"
)

func TestHTTPHealthUsesDurableCoverageAndQuality(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/http.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	emptyMeter := New(nopSource{}, Options{Recorder: store})
	handler := NewHTTPHandler(emptyMeter, store)

	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("empty health status = %d, want 503", response.Code)
	}
	assertJSONResponse(t, response)

	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/summary", nil))
	if response.Code != http.StatusBadRequest {
		t.Fatalf("invalid summary status = %d, want 400", response.Code)
	}
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/v1/status", nil))
	if response.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST status = %d, want 405", response.Code)
	}

	start := time.Unix(1_700_000_000, 0).UTC()
	seedCoveredRun(t, store, start, 2, func(second int, _ uint32, _ int64) (int, []Stat) {
		return 1, nil
	})
	readyMeter := New(&modeledSource{latest: 4}, Options{Recorder: store})
	handler = NewHTTPHandler(readyMeter, store)
	query := url.Values{
		"start":       {start.Format(time.RFC3339)},
		"end":         {start.Add(2 * time.Second).Format(time.RFC3339)},
		"window_size": {"1"},
	}
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/summary?"+query.Encode(), nil))
	var summary SummaryResult
	if err := json.Unmarshal(response.Body.Bytes(), &summary); err != nil {
		t.Fatal(err)
	}
	if summary.Complete {
		t.Fatal("unverified startup source reported complete data")
	}
	readyMeter.pollOnce(context.Background())
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("covered health status = %d, want 200: %s", response.Code, response.Body.String())
	}

	readyMeter.mu.Lock()
	readyMeter.skippedMC = 1
	readyMeter.mu.Unlock()
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("runtime skip health status = %d, want 503", response.Code)
	}
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/summary?"+query.Encode(), nil))
	summary = SummaryResult{}
	if err := json.Unmarshal(response.Body.Bytes(), &summary); err != nil {
		t.Fatal(err)
	}
	if summary.Complete {
		t.Fatal("runtime skip did not invalidate summary")
	}

	readyMeter.mu.Lock()
	readyMeter.skippedMC = 0
	readyMeter.mu.Unlock()
	if _, err := store.RecordBlock(Stat{
		ID: BlockID{Workchain: 0, Shard: testRootShard + 1, Seqno: 99}, GenUtime: start.Unix(), Partial: true,
	}); err != nil {
		t.Fatal(err)
	}
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("partial health status = %d, want 503", response.Code)
	}

	pendingMeter := New(nopSource{}, Options{Recorder: store, Backfill: true})
	pendingHandler := NewHTTPHandler(pendingMeter, store)
	response = httptest.NewRecorder()
	pendingHandler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("pending backfill health status = %d, want 503", response.Code)
	}
}

func TestHTTPStatusReportsMismatchTime(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/mismatch.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err := store.MarkRollback(); err != nil {
		t.Fatal(err)
	}
	handler := NewHTTPHandler(New(nopSource{}, Options{Recorder: store}), store)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/status", nil))
	var status struct {
		LastMismatchAt *time.Time `json:"last_mismatch_at"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &status); err != nil {
		t.Fatal(err)
	}
	if response.Code != http.StatusOK || status.LastMismatchAt == nil || time.Since(*status.LastMismatchAt) > time.Minute {
		t.Fatalf("mismatch timestamp missing from status: %d %s", response.Code, response.Body.String())
	}
}

func TestPersistentReopenHydratesStatusWindows(t *testing.T) {
	path := t.TempDir() + "/windows.db"
	store, err := OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	start := time.Unix(1_700_400_000, 0).UTC()
	seedCoveredRun(t, store, start, 5, func(second int, _ uint32, _ int64) (int, []Stat) {
		return 1, []Stat{{
			ID:      BlockID{Workchain: 1, Shard: 2, Seqno: uint32(second + 1)},
			TxCount: 2,
		}}
	})
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = OpenStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	meter := New(&modeledSource{latest: 7}, Options{Recorder: store, Windows: []int{5}})
	snapshot := meter.Snapshot()
	if !snapshot.WindowsReady || len(snapshot.Windows) != 1 || snapshot.Windows[0].TotalTx != 15 || snapshot.Windows[0].Blocks != 15 {
		t.Fatalf("rehydrated snapshot windows: %+v", snapshot)
	}
	meter.pollOnce(context.Background())
	handler := NewHTTPHandler(meter, store)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/status", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}
	var status struct {
		Windows []WindowTPS `json:"windows"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &status); err != nil {
		t.Fatal(err)
	}
	if len(status.Windows) != 1 || status.Windows[0].TotalTx != 15 || status.Windows[0].Blocks != 15 {
		t.Fatalf("rehydrated status windows: %+v", status.Windows)
	}
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("hydrated health = %d, want 200: %s", response.Code, response.Body.String())
	}
}

func TestHistoricalQueryIgnoresCurrentRuntimeQuality(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/historical-quality.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	start := time.Unix(1_700_500_000, 0).UTC()
	seedCoveredRun(t, store, start, 2, func(_ int, _ uint32, _ int64) (int, []Stat) {
		return 4, nil
	})
	if err := store.ResetLiveChain(); err != nil {
		t.Fatal(err)
	}
	meter := New(nopSource{}, Options{Recorder: store})
	meter.mu.Lock()
	meter.skippedMC = 1
	meter.mu.Unlock()
	handler := NewHTTPHandler(meter, store)
	query := url.Values{
		"start":       {start.Format(time.RFC3339)},
		"end":         {start.Add(2 * time.Second).Format(time.RFC3339)},
		"window_size": {"1"},
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/summary?"+query.Encode(), nil))
	if response.Code != http.StatusOK {
		t.Fatalf("historical summary status = %d: %s", response.Code, response.Body.String())
	}
	var summary SummaryResult
	if err := json.Unmarshal(response.Body.Bytes(), &summary); err != nil {
		t.Fatal(err)
	}
	if !summary.Complete || summary.IncludesActiveEpoch || len(summary.Epochs) != 1 || summary.Epochs[0] != 1 {
		t.Fatalf("current runtime quality leaked into historical summary: %+v", summary)
	}
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("empty active epoch health = %d, want 503", response.Code)
	}
}

func TestHTTPRangeNormalizesFractionalSeconds(t *testing.T) {
	store, err := OpenStore(t.TempDir() + "/fractional-range.db")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	start := time.Unix(1_700_600_000, 0).UTC()
	seedCoveredRun(t, store, start, 2, func(_ int, _ uint32, _ int64) (int, []Stat) {
		return 1, nil
	})
	meter := New(&modeledSource{latest: 4}, Options{Recorder: store})
	meter.pollOnce(context.Background())
	handler := NewHTTPHandler(meter, store)
	query := url.Values{
		"start":       {start.Add(449 * time.Millisecond).Format(time.RFC3339Nano)},
		"end":         {start.Add(2*time.Second + 449*time.Millisecond).Format(time.RFC3339Nano)},
		"window_size": {"1"},
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/series?"+query.Encode(), nil))
	if response.Code != http.StatusOK {
		t.Fatalf("fractional range status = %d: %s", response.Code, response.Body.String())
	}
	var series SeriesResult
	if err := json.Unmarshal(response.Body.Bytes(), &series); err != nil {
		t.Fatal(err)
	}
	if !series.Start.Equal(start) || !series.End.Equal(start.Add(2*time.Second)) || len(series.Points) != 2 {
		t.Fatalf("normalized fractional range: %+v", series)
	}
}

func assertJSONResponse(t *testing.T, response *httptest.ResponseRecorder) {
	t.Helper()
	if got := response.Header().Get("Content-Type"); got != "application/json" {
		t.Fatalf("Content-Type = %q, want application/json", got)
	}
	var body any
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("response is not JSON: %v: %q", err, response.Body.String())
	}
}
