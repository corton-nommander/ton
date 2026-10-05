// tpsmeter — Go port of the Python TON TPS meter (tpsmeter.py).
//
// Polls a Toncenter v2 HTTP API, walks new masterchain blocks + the basechain
// shard blocks they reference, counts transactions, and prints one status line
// per tick: the active shard count (the split detector) and sliding-window TPS
// at 1/5/15 min. Blocks/per-shard detail from the Python dashboard are dropped —
// only shards + TPS matter for the split-load test.
//
// The measurement engine lives in pkg/tpsmeter and is source-agnostic: cmd/spam
// will reuse it for a deterministic "control run" (drive load 15 min, then read
// TPS(600) = the mean TPS over the last 10 min).
//
// Two block sources: a Toncenter v2 HTTP source (default) and a liteserver source
// (--ls-config). LS is preferred on devnet (no toncenter) and wherever you want
// to read straight off validators.
//
// Flags (env in parentheses; flag > env > default):
//
//	-l, --ls-config (LS_CONFIG)           TON global config JSON -> use the liteserver source
//	--api           (TONCENTER_ENDPOINT)  HTTP API base URL (default http://devnet-01.toncenter.com)
//	--api-key       (TONCENTER_API_KEY)   optional X-API-Key (lifts the public RPS cap); HTTP only
//	--catchup                              on start, also walk BACKWARD to backfill history to the
//	                                       largest window (default true) so 1/5/15m are populated at once
//	--interval                             masterchain poll interval seconds (default 0.5)
//	--print                                stdout line interval seconds (default 2)
//	--windows                              sliding TPS windows, comma sec (default 60,300,900)
//	--workers                              parallel fetchers, for both the forward walk and backfill
//	                                       (default 16). The forward catch-up is batched, so this is the
//	                                       knob that sets how fast a lagging meter regains the chain head
//	                                       — raise it (with --rps) when it falls behind under load.
//	--rps                                  outbound RPS cap on ALL requests, 0 = unlimited (default 200)
//	--history                              max blocks kept in memory (default 65536)
//	--listen                               JSON API bind address; empty keeps stdout monitor mode
//	--store                                multi-run bbolt history file (required with --listen)
//	--reset-chain                          rotate --store to a fresh live-chain epoch and exit
//	--import-store                         import another history DB into --store and exit
//	--summary                              query exact summaries from --store and exit
//	--start / --end                        whole-second RFC3339 interval for --summary
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/tpsmeter"
)

const defaultAPI = "http://devnet-01.toncenter.com"

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "tpsmeter:", err)
		os.Exit(1)
	}
}

func run() error {
	return runArgs(os.Args[1:])
}

func runArgs(args []string) error {
	flags := flag.NewFlagSet("tpsmeter", flag.ContinueOnError)
	var (
		api        = flags.String("api", envOr("TONCENTER_ENDPOINT", defaultAPI), "Toncenter v2 API base URL")
		apiKey     = flags.String("api-key", os.Getenv("TONCENTER_API_KEY"), "optional X-API-Key")
		interval   = flags.Float64("interval", 0.5, "masterchain poll interval, seconds")
		printInt   = flags.Float64("print", 2.0, "stdout line interval, seconds")
		windows    = flags.String("windows", "60,300,900", "sliding TPS windows, comma-separated seconds")
		workers    = flags.Int("workers", 16, "parallel fetchers for the forward catch-up and backfill")
		rps        = flags.Float64("rps", 200, "cap outbound API RPS on all requests (0 = unlimited); protects the LS/TC during catch-up")
		history    = flags.Int("history", 65536, "max blocks kept in memory")
		catchup    = flags.Bool("catchup", true, "on start, also walk backward to backfill history to the largest window")
		listen     = flags.String("listen", "", "JSON API bind address (for example 127.0.0.1:8091); empty runs the stdout monitor")
		storePath  = flags.String("store", "", "multi-run bbolt history file (required with --listen)")
		resetChain = flags.Bool("reset-chain", false, "rotate the store to a fresh live-chain epoch and exit")
		importPath = flags.String("import-store", "", "import another tpsmeter history DB into --store and exit")
		summary    = flags.Bool("summary", false, "query exact summaries from --store and exit")
		startRaw   = flags.String("start", "", "summary interval start (whole-second RFC3339)")
		endRaw     = flags.String("end", "", "summary interval end (whole-second RFC3339)")
	)
	// LS source: --ls-config / -l (env LS_CONFIG). When set, use the liteserver
	// source instead of the HTTP one.
	var lsConfig string
	flags.StringVar(&lsConfig, "ls-config", os.Getenv("LS_CONFIG"), "TON global config JSON -> use liteserver source")
	flags.StringVar(&lsConfig, "l", os.Getenv("LS_CONFIG"), "TON global config JSON (shorthand for --ls-config)")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return fmt.Errorf("unexpected positional arguments: %s", strings.Join(flags.Args(), " "))
	}
	if *resetChain && *importPath != "" {
		return fmt.Errorf("--reset-chain cannot be combined with --import-store")
	}
	if *resetChain {
		if err := validateMaintenanceFlags(flags, *storePath, "reset-chain"); err != nil {
			return err
		}
		status, err := resetLiveChainStore(*storePath)
		if err != nil {
			return err
		}
		fmt.Printf("# tpsmeter reset chain store=%s active_epoch=%d\n", *storePath, status.Epoch)
		return nil
	}
	if *importPath != "" {
		if err := validateMaintenanceFlags(flags, *storePath, "import-store"); err != nil {
			return err
		}
		imported, err := importHistoryStore(*storePath, *importPath)
		if err != nil {
			return err
		}
		fmt.Printf("# tpsmeter imported store=%s source=%s epochs=%v\n", *storePath, *importPath, imported)
		return nil
	}
	if *summary {
		if err := validateSummaryFlags(flags, *storePath, *startRaw, *endRaw); err != nil {
			return err
		}
		wins, err := parseWindows(*windows)
		if err != nil {
			return fmt.Errorf("invalid --windows: %w", err)
		}
		summaries, err := queryStoreSummaries(*storePath, *startRaw, *endRaw, wins)
		if err != nil {
			return err
		}
		return json.NewEncoder(os.Stdout).Encode(summaries)
	}
	if *startRaw != "" || *endRaw != "" {
		return fmt.Errorf("--start and --end require --summary")
	}

	wins, err := parseWindows(*windows)
	if err != nil {
		return fmt.Errorf("invalid --windows: %w", err)
	}
	if *listen != "" && *storePath == "" {
		return fmt.Errorf("--store is required with --listen")
	}
	if *listen != "" {
		if err := validateListenAddress(*listen); err != nil {
			return fmt.Errorf("invalid --listen: %w", err)
		}
	}
	if *interval <= 0 || *printInt <= 0 {
		return fmt.Errorf("--interval and --print must be positive")
	}
	if *workers <= 0 || *history <= 0 {
		return fmt.Errorf("--workers and --history must be positive")
	}

	var src tpsmeter.Source
	if lsConfig != "" {
		ls, err := tpsmeter.NewLSSourceFromConfig(lsConfig, *rps)
		if err != nil {
			return fmt.Errorf("ls source: %w", err)
		}
		src = ls
	} else {
		src = tpsmeter.NewToncenterSource(tpsmeter.ToncenterConfig{
			Base:    *api,
			APIKey:  *apiKey,
			RPS:     *rps,
			MaxConn: *workers * 2,
		})
	}
	defer closeSource(src)

	var store *tpsmeter.Store
	var recorder tpsmeter.Recorder
	if *storePath != "" {
		store, err = tpsmeter.OpenStore(*storePath)
		if err != nil {
			return err
		}
		defer store.Close()
		recorder = store
	}
	meter := tpsmeter.New(src, tpsmeter.Options{
		PollInterval: durSec(*interval),
		Windows:      wins,
		Workers:      *workers,
		HistoryMax:   *history,
		Backfill:     *catchup,
		Recorder:     recorder,
	})

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if *listen != "" {
		return runDaemon(ctx, meter, store, *listen)
	}
	return runMonitor(ctx, meter, src, *interval, *windows, *workers, *printInt)
}

func validateMaintenanceFlags(flags *flag.FlagSet, storePath, operation string) error {
	if storePath == "" {
		return fmt.Errorf("--store is required with --%s", operation)
	}
	var incompatible string
	flags.Visit(func(value *flag.Flag) {
		if value.Name != operation && value.Name != "store" && incompatible == "" {
			incompatible = value.Name
		}
	})
	if incompatible != "" {
		return fmt.Errorf("--%s cannot be combined with --%s", operation, incompatible)
	}
	return nil
}

func validateSummaryFlags(flags *flag.FlagSet, storePath, start, end string) error {
	if storePath == "" {
		return fmt.Errorf("--store is required with --summary")
	}
	if start == "" || end == "" {
		return fmt.Errorf("--start and --end are required with --summary")
	}
	allowed := map[string]bool{"summary": true, "store": true, "start": true, "end": true, "windows": true}
	var incompatible string
	flags.Visit(func(value *flag.Flag) {
		if !allowed[value.Name] && incompatible == "" {
			incompatible = value.Name
		}
	})
	if incompatible != "" {
		return fmt.Errorf("--summary cannot be combined with --%s", incompatible)
	}
	return nil
}

func queryStoreSummaries(path, startRaw, endRaw string, windows []int) ([]tpsmeter.SummaryResult, error) {
	if _, err := os.Stat(path); err != nil {
		return nil, fmt.Errorf("stat store %s: %w", path, err)
	}
	start, err := time.Parse(time.RFC3339Nano, startRaw)
	if err != nil {
		return nil, fmt.Errorf("invalid --start: %w", err)
	}
	end, err := time.Parse(time.RFC3339Nano, endRaw)
	if err != nil {
		return nil, fmt.Errorf("invalid --end: %w", err)
	}
	if start.Nanosecond() != 0 || end.Nanosecond() != 0 {
		return nil, fmt.Errorf("--start and --end must align to whole seconds")
	}

	store, err := tpsmeter.OpenStore(path)
	if err != nil {
		return nil, err
	}
	results := make([]tpsmeter.SummaryResult, 0, len(windows))
	for _, window := range windows {
		result, summaryErr := store.Summary(start, end, window)
		if summaryErr != nil {
			_ = store.Close()
			return nil, fmt.Errorf("summary window %d: %w", window, summaryErr)
		}
		results = append(results, result)
	}
	if err := store.Close(); err != nil {
		return nil, err
	}
	return results, nil
}

func resetLiveChainStore(path string) (tpsmeter.StoreStatus, error) {
	store, err := tpsmeter.OpenStore(path)
	if err != nil {
		return tpsmeter.StoreStatus{}, err
	}
	if err := store.ResetLiveChain(); err != nil {
		_ = store.Close()
		return tpsmeter.StoreStatus{}, err
	}
	status, statusErr := store.Status()
	closeErr := store.Close()
	if statusErr != nil {
		return tpsmeter.StoreStatus{}, statusErr
	}
	if closeErr != nil {
		return tpsmeter.StoreStatus{}, closeErr
	}
	return status, nil
}

func importHistoryStore(path, sourcePath string) ([]uint64, error) {
	store, err := tpsmeter.OpenStore(path)
	if err != nil {
		return nil, err
	}
	imported, importErr := store.ImportStore(sourcePath)
	closeErr := store.Close()
	if importErr != nil {
		return nil, importErr
	}
	if closeErr != nil {
		return nil, closeErr
	}
	return imported, nil
}

func runMonitor(ctx context.Context, meter *tpsmeter.Meter, src tpsmeter.Source, interval float64, windows string, workers int, printInterval float64) error {
	meterDone := make(chan error, 1)
	go func() { meterDone <- meter.Run(ctx) }()

	fmt.Printf("# tpsmeter %s  poll=%.1fs windows=%s workers=%d\n",
		src.Description(), interval, windows, workers)

	ticker := time.NewTicker(durSec(printInterval))
	defer ticker.Stop()
	prevShards := -1
	for {
		select {
		case <-ctx.Done():
			err := <-meterDone
			fmt.Println()
			printFinal(meter.Snapshot())
			if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
				return nil
			}
			return err
		case err := <-meterDone:
			printFinal(meter.Snapshot())
			if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
				return nil
			}
			return err
		case <-ticker.C:
			snap := meter.Snapshot()
			if snap.Blocks == 0 && snap.StoredBlocks == 0 {
				fmt.Printf("%s  waiting for first block…\n", nowStamp())
				continue
			}
			evt := ""
			if snap.Shards >= 1 && prevShards >= 1 && snap.Shards != prevShards {
				if snap.Shards > prevShards {
					evt = fmt.Sprintf("   >>> SPLIT %d -> %d <<<", prevShards, snap.Shards)
				} else {
					evt = fmt.Sprintf("   <<< MERGE %d -> %d >>>", prevShards, snap.Shards)
				}
			}
			if snap.Shards >= 1 {
				prevShards = snap.Shards
			}
			fmt.Printf("%s  %s%s\n", nowStamp(), snap.String(), evt)
			if snap.LastErr != "" && snap.Errors > 0 {
				// keep errors visible but unobtrusive
			}
		}
	}
}

func runDaemon(parent context.Context, meter *tpsmeter.Meter, store *tpsmeter.Store, listen string) error {
	ctx, cancel := context.WithCancel(parent)
	defer cancel()

	server := &http.Server{
		Addr:              listen,
		Handler:           tpsmeter.NewHTTPHandler(meter, store),
		ReadHeaderTimeout: 5 * time.Second,
		IdleTimeout:       time.Minute,
	}
	meterDone := make(chan error, 1)
	serverDone := make(chan error, 1)
	go func() { meterDone <- meter.Run(ctx) }()
	go func() { serverDone <- server.ListenAndServe() }()

	fmt.Printf("# tpsmeter daemon %s listen=%s store=%s\n", meter.Source().Description(), listen, store.Path())
	var runErr error
	meterFinished, serverFinished := false, false
	select {
	case <-parent.Done():
	case err := <-meterDone:
		meterFinished = true
		if err != nil && !errors.Is(err, context.Canceled) && !errors.Is(err, context.DeadlineExceeded) {
			runErr = fmt.Errorf("meter: %w", err)
		}
	case err := <-serverDone:
		serverFinished = true
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			runErr = fmt.Errorf("http server: %w", err)
		}
	}

	cancel()
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer shutdownCancel()
	if err := server.Shutdown(shutdownCtx); err != nil && runErr == nil {
		runErr = fmt.Errorf("http shutdown: %w", err)
	}
	if !serverFinished {
		if err := <-serverDone; err != nil && !errors.Is(err, http.ErrServerClosed) && runErr == nil {
			runErr = fmt.Errorf("http server: %w", err)
		}
	}
	if !meterFinished {
		if err := <-meterDone; err != nil && !errors.Is(err, context.Canceled) && !errors.Is(err, context.DeadlineExceeded) && runErr == nil {
			runErr = fmt.Errorf("meter: %w", err)
		}
	}
	return runErr
}

func closeSource(src tpsmeter.Source) {
	if closer, ok := src.(interface{ Close() }); ok {
		closer.Close()
	}
}

func printFinal(s tpsmeter.Snapshot) {
	fmt.Println("# final:")
	fmt.Printf("#   shards=%d  span=%ds  tx=%d  polls=%d  errors=%d\n",
		s.Shards, s.SpanSec, s.TotalTx, s.Polls, s.Errors)
	for _, w := range s.Windows {
		mark := ""
		if w.Filling {
			mark = "  (filling — span < window)"
		}
		fmt.Printf("#   %-4s TPS=%.2f  blk/s mc=%.2f wc=%.2f  (%d tx / %d blocks: %d mc, %d wc)%s\n",
			winName(w.WindowSec), w.TPS, w.MCBlockRate, w.WCBlockRate,
			w.TotalTx, w.Blocks, w.MCBlocks, w.WCBlocks, mark)
	}
	if s.LastErr != "" {
		fmt.Printf("#   last error: %s\n", s.LastErr)
	}
}

func winName(sec int) string {
	if sec%60 == 0 {
		return fmt.Sprintf("%dm", sec/60)
	}
	return fmt.Sprintf("%ds", sec)
}

func nowStamp() string { return time.Now().Format("15:04:05") }

func durSec(s float64) time.Duration { return time.Duration(s * float64(time.Second)) }

func validateListenAddress(address string) error {
	host, portRaw, err := net.SplitHostPort(address)
	if err != nil {
		return err
	}
	port, err := strconv.Atoi(portRaw)
	if err != nil || port < 1 || port > 65535 {
		return fmt.Errorf("port must be between 1 and 65535")
	}
	if host == "localhost" {
		return nil
	}
	ip := net.ParseIP(host)
	if ip == nil || !ip.IsLoopback() {
		return fmt.Errorf("host %q is not loopback", host)
	}
	return nil
}

func parseWindows(s string) ([]int, error) {
	var out []int
	for _, p := range strings.Split(s, ",") {
		p = strings.TrimSpace(p)
		if p == "" {
			continue
		}
		v, err := strconv.Atoi(p)
		if err != nil {
			return nil, err
		}
		if v <= 0 {
			return nil, fmt.Errorf("window %q must be positive", p)
		}
		out = append(out, v)
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no windows")
	}
	return out, nil
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
