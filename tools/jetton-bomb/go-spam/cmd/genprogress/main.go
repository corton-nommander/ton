// genprogress — live ANSI dashboard for bench-state-gen ("DAN basechain")
// generation running across many remote nodes during `ton start`.
//
// Each tick it fans out one ssh round-trip per node that cats the node's
// progress.json + async.rc (co-located in --gen-dir), folds the results into a
// per-node model, and redraws an in-place dashboard: an aggregate bar driven by
// the SLOWEST node, a phase legend, and one compact line per node (bar / %% /
// phase, with stalled / done / FAILED states). If stdout is not a tty it
// prints one plain summary line per tick instead.
//
// It is pure UX: the measurement/parse/render core lives in core.go and is
// network-free + unit-tested. ton runs the authoritative wait afterwards, so
// exit codes here are advisory: 0 when every node's async.rc == 0; non-zero if
// any node failed (its async.log tail is printed first), on --timeout, or on
// SIGINT (which kills nothing).
//
// Flags (see PROGRESS_CONTRACT.md §2):
//
//	--hosts-file <file>     required; counted-hosts format (line 1 may be an integer count) OR plain list
//	--gen-dir <dir>         required, e.g. /tmp/zerostate/dan-basechain-gen
//	--ssh-user <user>       default: $USER
//	--local-file <path>     optional; also show this machine's own progress.json (read directly), labeled "local"
//	--interval <sec>        default 3
//	--timeout <sec>         default 3600; overall cap
//	--accounts-total <n>    optional header hint; else taken from a node snapshot
//	--no-color              disable ANSI colors
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"os/user"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
)

const (
	defaultGenDir = "/tmp/zerostate/dan-basechain-gen"
	sshTimeout    = 15 * time.Second // per-node hard cap on a tick's ssh
	logTailLines  = 40
)

var (
	errTimeout     = errors.New("timeout: generation did not finish within --timeout")
	errInterrupted = errors.New("interrupted")
	errNodeFailed  = errors.New("one or more nodes failed")
)

func main() {
	if err := runArgs(os.Args[1:], os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, "genprogress:", err)
		os.Exit(1)
	}
}

// nodeSpec identifies one node to poll: a remote ssh host, or the local machine.
type nodeSpec struct {
	host      string // map key + display fallback
	label     string // display override (e.g. "local")
	local     bool
	localFile string // progress.json path for local nodes
}

func runArgs(args []string, out *os.File) error {
	flags := flag.NewFlagSet("genprogress", flag.ContinueOnError)
	var (
		hostsFile = flags.String("hosts-file", "", "hosts file (counted-hosts or plain list); required")
		genDir    = flags.String("gen-dir", defaultGenDir, "generation dir on each node (holds progress.json/async.rc)")
		sshUser   = flags.String("ssh-user", defaultUser(), "ssh user (default $USER)")
		localFile = flags.String("local-file", "", "also show this machine's progress.json (read directly), labeled local")
		interval  = flags.Float64("interval", 3, "poll interval seconds")
		timeout   = flags.Float64("timeout", 3600, "overall timeout seconds")
		accTotal  = flags.Int64("accounts-total", 0, "header hint for total accounts (else from a snapshot)")
		noColor   = flags.Bool("no-color", false, "disable ANSI colors")
	)
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return fmt.Errorf("unexpected positional arguments: %s", strings.Join(flags.Args(), " "))
	}
	if *hostsFile == "" {
		return fmt.Errorf("--hosts-file is required")
	}
	if *genDir == "" {
		return fmt.Errorf("--gen-dir is required")
	}
	if *interval <= 0 {
		return fmt.Errorf("--interval must be positive")
	}
	if *timeout <= 0 {
		return fmt.Errorf("--timeout must be positive")
	}

	data, err := os.ReadFile(*hostsFile)
	if err != nil {
		return fmt.Errorf("read hosts file: %w", err)
	}
	hosts := ParseHostsFile(string(data))

	var specs []nodeSpec
	if *localFile != "" {
		specs = append(specs, nodeSpec{host: "local", label: "local", local: true, localFile: *localFile})
	}
	for _, h := range hosts {
		specs = append(specs, nodeSpec{host: h})
	}
	if len(specs) == 0 {
		return fmt.Errorf("no hosts in %s (and no --local-file)", *hostsFile)
	}

	dir := strings.TrimRight(*genDir, "/")
	opts := Options{
		Title:         "DAN basechain generation",
		AccountsTotal: *accTotal,
		Interval:      durSec(*interval),
		Color:         !*noColor && isTTY(out),
		BarWidth:      20,
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	loop := &loopState{
		specs:   specs,
		user:    *sshUser,
		genDir:  dir,
		opts:    opts,
		out:     out,
		tty:     isTTY(out),
		tracker: NewTracker(),
	}
	return loop.run(ctx, durSec(*interval), durSec(*timeout))
}

type loopState struct {
	specs   []nodeSpec
	user    string
	genDir  string
	opts    Options
	out     *os.File
	tty     bool
	tracker *Tracker

	screen screen // in-place redraw bookkeeping (tty only)
}

func (l *loopState) run(ctx context.Context, interval, timeout time.Duration) error {
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	deadline := time.After(timeout)

	for {
		now := time.Now()
		samples := l.pullAll(ctx)
		for _, s := range samples {
			l.tracker.Observe(s, now)
		}
		vm := Aggregate(l.tracker.States(), l.opts)
		l.draw(vm)

		if vm.AnyFailed {
			l.reportFailures(ctx, vm)
			return errNodeFailed
		}
		if vm.AllDone {
			return nil
		}

		select {
		case <-ctx.Done():
			return errInterrupted
		case <-deadline:
			return errTimeout
		case <-ticker.C:
		}
	}
}

// draw renders a tty frame in place, or a plain one-line summary otherwise.
func (l *loopState) draw(vm ViewModel) {
	if l.tty {
		l.screen.draw(l.out, Render(vm, l.opts))
		return
	}
	fmt.Fprintln(l.out, summaryLine(vm))
}

func summaryLine(vm ViewModel) string {
	slow := vm.SlowestHost
	if slow == "" {
		slow = "-"
	} else if vm.SlowestPhase != "" {
		slow += "(" + vm.SlowestPhase + ")"
	}
	return fmt.Sprintf("%s overall %s slowest=%s done=%d/%d running=%d starting=%d failed=%d gen=%s",
		time.Now().Format("15:04:05"),
		pctStr(vm.AggPct), slow,
		vm.DoneCount, vm.NodeCount, vm.RunningCount, vm.StartingCount, vm.FailedCount,
		fmtDurSec(vm.GenElapsedSec))
}

// reportFailures prints each failed node's async.log tail below the last frame.
func (l *loopState) reportFailures(ctx context.Context, vm ViewModel) {
	specByHost := map[string]nodeSpec{}
	for _, s := range l.specs {
		specByHost[s.host] = s
	}
	for _, n := range vm.Nodes {
		if n.Status != StatusFailed {
			continue
		}
		fmt.Fprintf(l.out, "\n=== %s FAILED (rc %d) — %s/async.log tail ===\n", n.Label, n.RC, l.genDir)
		tail := l.fetchLogTail(ctx, specByHost[n.Host])
		if strings.TrimSpace(tail) == "" {
			fmt.Fprintln(l.out, "(no async.log content)")
		} else {
			fmt.Fprintln(l.out, strings.TrimRight(tail, "\n"))
		}
	}
}

// ---------------------------------------------------------------------------
// ssh puller (impure).
// ---------------------------------------------------------------------------

// remoteCmd is EXACTLY the combined command from PROGRESS_CONTRACT.md §2.
func remoteCmd(genDir string) string {
	return fmt.Sprintf(`sudo cat %s/progress.json 2>/dev/null; echo "---RC---"; sudo cat %s/async.rc 2>/dev/null`, genDir, genDir)
}

// sshFlags are EXACTLY the flags from the contract (match ton's `remote`).
func sshFlags() []string {
	return []string{
		"-o", "BatchMode=yes",
		"-o", "ConnectTimeout=10",
		"-o", "StrictHostKeyChecking=no",
		"-o", "UserKnownHostsFile=/dev/null",
		"-o", "LogLevel=ERROR",
	}
}

func (l *loopState) pullAll(ctx context.Context) []Sample {
	samples := make([]Sample, len(l.specs))
	var wg sync.WaitGroup
	for i, spec := range l.specs {
		wg.Add(1)
		go func(i int, spec nodeSpec) {
			defer wg.Done()
			if spec.local {
				samples[i] = pullLocal(spec)
			} else {
				samples[i] = l.pullRemote(ctx, spec)
			}
		}(i, spec)
	}
	wg.Wait()
	return samples
}

func (l *loopState) pullRemote(ctx context.Context, spec nodeSpec) Sample {
	s := Sample{Host: spec.host, Label: spec.label}
	cctx, cancel := context.WithTimeout(ctx, sshTimeout)
	defer cancel()

	args := append(sshFlags(), l.user+"@"+spec.host, remoteCmd(l.genDir))
	cmd := exec.CommandContext(cctx, "ssh", args...)
	stdout, err := cmd.Output()
	if err != nil {
		// ssh transport failure (exit 255), spawn failure, or timeout → unreachable.
		// A non-255 exit (e.g. 1 when async.rc is absent) still gives us stdout.
		var exitErr *exec.ExitError
		if errors.As(err, &exitErr) && exitErr.ExitCode() != 255 {
			// command ran; parse whatever stdout we got.
		} else {
			s.SSHFailed = true
			return s
		}
	}
	s.RawProgress, s.RawRC = splitOutput(string(stdout))
	return s
}

func pullLocal(spec nodeSpec) Sample {
	s := Sample{Host: spec.host, Label: spec.label}
	if b, err := os.ReadFile(spec.localFile); err == nil {
		s.RawProgress = strings.TrimSpace(string(b))
	}
	rcPath := filepath.Join(filepath.Dir(spec.localFile), "async.rc")
	if b, err := os.ReadFile(rcPath); err == nil {
		s.RawRC = strings.TrimSpace(string(b))
	}
	return s
}

// fetchLogTail pulls the tail of a failed node's async.log for the report.
func (l *loopState) fetchLogTail(ctx context.Context, spec nodeSpec) string {
	cctx, cancel := context.WithTimeout(ctx, sshTimeout)
	defer cancel()
	if spec.local {
		b, err := os.ReadFile(filepath.Join(filepath.Dir(spec.localFile), "async.log"))
		if err != nil {
			return ""
		}
		return lastLines(string(b), logTailLines)
	}
	cmd := exec.CommandContext(cctx, "ssh",
		append(sshFlags(), l.user+"@"+spec.host,
			fmt.Sprintf("sudo tail -n %d %s/async.log 2>/dev/null", logTailLines, l.genDir))...)
	out, _ := cmd.Output()
	return string(out)
}

// splitOutput separates the combined ssh output into progress.json / async.rc
// around the "---RC---" marker. Robust to the marker having no leading newline.
func splitOutput(out string) (prog, rc string) {
	parts := strings.SplitN(out, "---RC---", 2)
	prog = strings.TrimSpace(parts[0])
	if len(parts) == 2 {
		rc = strings.TrimSpace(parts[1])
	}
	return prog, rc
}

func lastLines(s string, n int) string {
	lines := strings.Split(strings.TrimRight(s, "\n"), "\n")
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	return strings.Join(lines, "\n")
}

// ---------------------------------------------------------------------------
// tty in-place redraw.
// ---------------------------------------------------------------------------

type screen struct{ prevLines int }

func (s *screen) draw(out *os.File, frame string) {
	if s.prevLines > 0 {
		fmt.Fprintf(out, "\x1b[%dA", s.prevLines) // cursor up N lines
		fmt.Fprint(out, "\x1b[J")                 // clear from cursor to end of screen
	}
	fmt.Fprint(out, frame)
	fmt.Fprint(out, "\n")
	s.prevLines = strings.Count(frame, "\n") + 1
}

func isTTY(f *os.File) bool {
	fi, err := f.Stat()
	if err != nil {
		return false
	}
	return fi.Mode()&os.ModeCharDevice != 0
}

func defaultUser() string {
	if u := os.Getenv("USER"); u != "" {
		return u
	}
	if u, err := user.Current(); err == nil {
		return u.Username
	}
	return ""
}

func durSec(s float64) time.Duration { return time.Duration(s * float64(time.Second)) }
