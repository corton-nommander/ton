// nativespam sends real source-signed native payments from funded test accounts.
// It uses no validator-local producer, TVM wallet, or self-message contract.
package main

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"math"
	"math/big"
	"math/bits"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/nativepay"
	"github.com/victor/jetton-bomb-go-spam/pkg/nativerpc"
	"github.com/victor/jetton-bomb-go-spam/pkg/sidechain"
	"github.com/xssnick/tonutils-go/address"
)

type options struct {
	Config, WalletDir, Output                    string
	Sources, Outputs, Workers, BatchSize, Window int
	LaneDepth                                    int
	Amount, Fee                                  uint64
	Rate                                         float64
	Duration, Warmup, DrainTimeout               time.Duration
}
type parent struct {
	nonce       uint64
	body        []byte
	lastAttempt time.Time
}
type source struct {
	sync.Mutex
	key                      ed25519.PrivateKey
	address, destination     *address.Address
	origin, issued, observed uint64
	maxNonce                 uint64
	pending                  []*parent
}
type counters struct {
	Parents, Attempts, Accepted, Rejected, Retries, RPCs, Transport atomic.Uint64
	mu                                                              sync.Mutex
	err                                                             error
}

func (c *counters) fail(err error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.err == nil {
		c.err = err
	}
}
func (c *counters) failure() error { c.mu.Lock(); defer c.mu.Unlock(); return c.err }

type counts struct {
	OfferedParents   uint64 `json:"offered_signed_parents"`
	OfferedPayments  uint64 `json:"offered_logical_payments"`
	AttemptedParents uint64 `json:"attempted_parents_including_retries"`
	Accepted         uint64 `json:"admission_acceptances_including_retries"`
	Rejected         uint64 `json:"admission_rejections_including_retries"`
	Retries          uint64 `json:"retried_parents"`
	RPCs             uint64 `json:"batch_rpc_attempts"`
	Transport        uint64 `json:"transport_errors"`
}

func (c *counters) snapshot(outputs int) counts {
	p := c.Parents.Load()
	return counts{p, p * uint64(outputs), c.Attempts.Load(), c.Accepted.Load(), c.Rejected.Load(), c.Retries.Load(), c.RPCs.Load(), c.Transport.Load()}
}

type audit struct {
	Source            string `json:"source"`
	Destination       string `json:"destination"`
	CommittedPayments uint64 `json:"committed_payments"`
	CompletedParents  uint64 `json:"completed_signed_parents"`
	BalanceVerified   bool   `json:"balances_verified"`
}
type report struct {
	Schema           int                 `json:"schema_version"`
	Workload         string              `json:"workload"`
	ProofPolicy      string              `json:"proof_policy"`
	Protocol         *sidechain.Report   `json:"protocol"`
	Options          options             `json:"options"`
	Initial          *nativerpc.Snapshot `json:"initial"`
	Start            *nativerpc.Snapshot `json:"start"`
	End              *nativerpc.Snapshot `json:"end"`
	Final            *nativerpc.Snapshot `json:"final"`
	Counts           counts              `json:"submission_counts"`
	MeasuredPayments uint64              `json:"measured_committed_payments"`
	MeasuredParents  uint64              `json:"measured_completed_signed_parents"`
	WallSeconds      float64             `json:"measurement_wall_seconds"`
	ChainSeconds     int64               `json:"measurement_chain_seconds"`
	PaymentTPS       float64             `json:"committed_payment_wall_tps"`
	ParentTPS        float64             `json:"completed_signed_parent_wall_tps"`
	ChainTPS         float64             `json:"committed_payment_chain_tps"`
	InternalMessages uint64              `json:"ton_internal_message_cells"`
	TotalPayments    uint64              `json:"total_committed_payments"`
	TotalParents     uint64              `json:"total_completed_signed_parents"`
	Audit            []audit             `json:"account_audit"`
	Drained          bool                `json:"drained"`
	Valid            bool                `json:"valid"`
	CapacityClaim    bool                `json:"capacity_claim"`
	Error            string              `json:"error,omitempty"`
}

func loadSources(dir string, n int) ([]*source, error) {
	result := make([]*source, n)
	seen := map[string]bool{}
	for i := range n {
		seed, e := os.ReadFile(filepath.Join(dir, fmt.Sprintf("source-%d.pk", i)))
		if e != nil {
			return nil, e
		}
		pub, e := os.ReadFile(filepath.Join(dir, fmt.Sprintf("source-%d.pub", i)))
		if e != nil {
			return nil, e
		}
		dst, e := os.ReadFile(filepath.Join(dir, fmt.Sprintf("dest-%d.pub", i)))
		if e != nil {
			return nil, e
		}
		if len(seed) != 32 || len(pub) != 32 || len(dst) != 32 {
			return nil, fmt.Errorf("source %d requires raw 32-byte seed/public keys", i)
		}
		key := ed25519.NewKeyFromSeed(seed)
		if !bytes.Equal(key.Public().(ed25519.PublicKey), pub) {
			return nil, fmt.Errorf("source %d seed/public key mismatch", i)
		}
		for _, id := range [][]byte{pub, dst} {
			h := hex.EncodeToString(id)
			if seen[h] {
				return nil, errors.New("sources and destinations must all be distinct for independent balance auditing")
			}
			seen[h] = true
		}
		result[i] = &source{key: key, address: nativepay.AddressFromKey(key), destination: address.NewAddress(0, 0, dst)}
	}
	return result, nil
}
func addresses(sources []*source, destinations bool) []*address.Address {
	a := make([]*address.Address, 0, len(sources)*2)
	for _, s := range sources {
		a = append(a, s.address)
	}
	if destinations {
		for _, s := range sources {
			a = append(a, s.destination)
		}
	}
	return a
}
func snapshot(ctx context.Context, c *nativerpc.Client, s []*source, all bool) (*nativerpc.Snapshot, error) {
	ctx, cancel := context.WithTimeout(ctx, 45*time.Second)
	defer cancel()
	return c.Snapshot(ctx, addresses(s, all), 32)
}

func requireTopology(snap *nativerpc.Snapshot, depth int) error {
	if snap == nil || depth < 1 || depth > 3 {
		return errors.New("native benchmark requires a depth 1..3 snapshot")
	}
	seen := map[uint64]bool{}
	for _, s := range snap.Shards {
		if s.Workchain != 0 {
			continue
		}
		id, e := strconv.ParseUint(s.Shard, 16, 64)
		if e != nil || id == 0 || 63-bits.TrailingZeros64(id) != depth || seen[id] {
			return errors.New("native topology changed or has duplicate/incomplete leaves")
		}
		seen[id] = true
	}
	if len(seen) != 1<<depth {
		return errors.New("native topology is not the requested complete fixed layout")
	}
	return nil
}
func applyObserved(sources []*source, snap *nativerpc.Snapshot, outputs int) error {
	if snap == nil || len(snap.Accounts) < len(sources) || outputs < 1 || outputs > 16 {
		return errors.New("invalid nonce snapshot or output count")
	}
	for i, s := range sources {
		if s == nil || s.address == nil || snap.Accounts[i].Address != s.address.StringRaw() {
			return errors.New("nonce snapshot account identity mismatch")
		}
		s.Lock()
		n := snap.Accounts[i].Nonce
		if n < s.observed || n > s.issued || (n-s.origin)%uint64(outputs) != 0 {
			s.Unlock()
			return fmt.Errorf("source %d observed nonce outside reserved interval", i)
		}
		s.observed = n
		for len(s.pending) > 0 && s.pending[0].nonce+uint64(outputs) <= n {
			s.pending[0] = nil
			s.pending = s.pending[1:]
		}
		s.Unlock()
	}
	return nil
}

func sendWorker(ctx context.Context, c *nativerpc.Client, sources []*source, domain []byte, depth uint32, o options, stats *counters, stopOffers bool) {
	cursor := 0
	for ctx.Err() == nil {
		bodies := make([][]byte, 0, o.BatchSize)
		for attempts := 0; attempts < len(sources)*2 && len(bodies) < o.BatchSize; attempts++ {
			s := sources[cursor%len(sources)]
			cursor++
			s.Lock()
			now := time.Now()
			var p *parent
			if len(s.pending) > 0 && now.Sub(s.pending[0].lastAttempt) >= 2*time.Second {
				p = s.pending[0]
				stats.Retries.Add(1)
			} else if !stopOffers && len(s.pending) < o.Window && s.issued <= s.maxNonce-uint64(o.Outputs) {
				outputs := make([]nativepay.Output, o.Outputs)
				for i := range outputs {
					outputs[i] = nativepay.Output{Destination: s.destination, Amount: o.Amount, Fee: o.Fee}
				}
				root, err := nativepay.BuildMessage(s.key, outputs, s.issued, uint32(now.Unix()+600), domain, true, depth)
				if err != nil {
					s.Unlock()
					stats.fail(err)
					fmt.Fprintln(os.Stderr, err)
					return
				}
				p = &parent{nonce: s.issued, body: root.ToBOCWithFlags(false)}
				s.issued += uint64(o.Outputs)
				s.pending = append(s.pending, p)
				stats.Parents.Add(1)
			}
			if p != nil {
				p.lastAttempt = now
				bodies = append(bodies, p.body)
			}
			s.Unlock()
		}
		if len(bodies) == 0 {
			select {
			case <-ctx.Done():
				return
			case <-time.After(10 * time.Millisecond):
			}
			continue
		}
		stats.Attempts.Add(uint64(len(bodies)))
		stats.RPCs.Add(1)
		rpcCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
		results, err := c.Send(rpcCtx, bodies)
		cancel()
		if err != nil {
			stats.Transport.Add(1)
			if ctx.Err() == nil {
				fmt.Fprintf(os.Stderr, "batch transport: %v\n", err)
			}
		} else {
			for _, r := range results {
				if r.Status == 1 {
					stats.Accepted.Add(1)
				} else {
					stats.Rejected.Add(1)
				}
			}
		}
		if o.Rate > 0 {
			// A per-worker ceiling in logical payment attempts, retries included.
			d := time.Duration(float64(len(bodies)*o.Outputs*o.Workers) / o.Rate * float64(time.Second))
			select {
			case <-ctx.Done():
				return
			case <-time.After(d):
			}
		}
	}
}
func launchWorkers(ctx context.Context, c *nativerpc.Client, sources []*source, domain []byte, depth uint32, o options, stats *counters, drain bool) *sync.WaitGroup {
	wg := &sync.WaitGroup{}
	for i := range o.Workers {
		part := []*source{}
		for j := i; j < len(sources); j += o.Workers {
			part = append(part, sources[j])
		}
		wg.Add(1)
		go func() { defer wg.Done(); sendWorker(ctx, c, part, domain, depth, o, stats, drain) }()
	}
	return wg
}
func auditInterval(before, after *nativerpc.Snapshot, sources []*source, o options) ([]audit, uint64, uint64, error) {
	if o.Outputs < 1 || o.Outputs > 16 || o.Amount == 0 || o.Fee > math.MaxUint64-o.Amount {
		return nil, 0, 0, errors.New("invalid audit payment parameters")
	}
	result := make([]audit, len(sources))
	var total, parents uint64
	if before == nil || after == nil || len(before.Accounts) != len(sources)*2 || len(after.Accounts) != len(sources)*2 {
		return nil, 0, 0, errors.New("incomplete account snapshots")
	}
	for i, s := range sources {
		if s == nil || s.address == nil || s.destination == nil {
			return nil, 0, 0, errors.New("missing audit source")
		}
		b, a := before.Accounts[i], after.Accounts[i]
		bd, ad := before.Accounts[len(sources)+i], after.Accounts[len(sources)+i]
		if b.Address != s.address.StringRaw() || a.Address != b.Address || bd.Address != s.destination.StringRaw() || ad.Address != bd.Address || a.Nonce < b.Nonce || b.Nonce < s.origin || a.Nonce > s.issued || (a.Nonce-s.origin)%uint64(o.Outputs) != 0 || (b.Nonce-s.origin)%uint64(o.Outputs) != 0 {
			return nil, 0, 0, errors.New("account identity/nonce mismatch")
		}
		n := a.Nonce - b.Nonce
		debit := new(big.Int).Mul(new(big.Int).SetUint64(n), new(big.Int).SetUint64(o.Amount+o.Fee))
		credit := new(big.Int).Mul(new(big.Int).SetUint64(n), new(big.Int).SetUint64(o.Amount))
		actualDebit := new(big.Int).Sub(new(big.Int).SetUint64(b.Balance), new(big.Int).SetUint64(a.Balance))
		actualCredit := new(big.Int).Sub(new(big.Int).SetUint64(ad.Balance), new(big.Int).SetUint64(bd.Balance))
		if debit.Cmp(actualDebit) != 0 || credit.Cmp(actualCredit) != 0 || bd.Nonce != ad.Nonce || b.Flags != a.Flags || bd.Flags != ad.Flags {
			return nil, 0, 0, fmt.Errorf("source %d debit/credit/flags conservation failed", i)
		}
		p := (a.Nonce-s.origin)/uint64(o.Outputs) - (b.Nonce-s.origin)/uint64(o.Outputs)
		result[i] = audit{s.address.StringRaw(), s.destination.StringRaw(), n, p, true}
		if n > math.MaxUint64-total || p > math.MaxUint64-parents {
			return nil, 0, 0, errors.New("aggregate audit counts overflow uint64")
		}
		total += n
		parents += p
	}
	return result, total, parents, nil
}

func run(ctx context.Context, o options) (r *report, err error) {
	r = &report{Schema: 1, Workload: "native_signed_payments_same_lane", ProofPolicy: "account_and_topology_merkle_proofs_against_local_liteserver_masterchain_root; consensus proof chain not independently verified", Options: o}
	defer func() {
		if err != nil {
			r.Error = err.Error()
			r.Valid = false
		}
	}()
	preCtx, cancel := context.WithTimeout(ctx, 45*time.Second)
	r.Protocol, err = sidechain.Inspect(preCtx, o.Config, 0)
	cancel()
	if err != nil {
		return r, err
	}
	if !r.Protocol.NativePaymentLanes || len(r.Protocol.BasechainShardDepths) != 1 || r.Protocol.BasechainShardDepths[0] != o.LaneDepth {
		return r, errors.New("nativespam requires active v16 native payment lanes and one fixed basechain depth")
	}
	depth := uint32(o.LaneDepth)
	if r.Protocol.BasechainShardCount != 1<<depth {
		return r, errors.New("incomplete fixed-lane topology; wait for genesis splits")
	}
	domain, err := hex.DecodeString(r.Protocol.ObservedZerostate.RootHash)
	if err != nil {
		return r, err
	}
	sources, err := loadSources(o.WalletDir, o.Sources)
	if err != nil {
		return r, err
	}
	for _, s := range sources {
		if _, err = nativepay.BuildMessage(s.key, []nativepay.Output{{Destination: s.destination, Amount: o.Amount, Fee: o.Fee}}, 0, uint32(time.Now().Unix()+600), domain, true, depth); err != nil {
			return r, err
		}
	}
	c, err := nativerpc.Connect(ctx, o.Config)
	if err != nil {
		return r, err
	}
	defer c.Close()
	r.Initial, err = snapshot(ctx, c, sources, true)
	if err != nil {
		return r, err
	}
	if err = requireTopology(r.Initial, o.LaneDepth); err != nil {
		return r, err
	}
	for i, s := range sources {
		s.origin = r.Initial.Accounts[i].Nonce
		s.issued = s.origin
		s.observed = s.origin
		available := r.Initial.Accounts[i].Balance / (o.Amount + o.Fee)
		if available > math.MaxUint64-s.origin {
			available = math.MaxUint64 - s.origin
		}
		s.maxNonce = s.origin + available
		if r.Initial.Accounts[i].Balance < uint64(o.Outputs)*(o.Amount+o.Fee) {
			return r, fmt.Errorf("source %d insufficient funded balance", i)
		}
	}
	stats := &counters{}
	loadCtx, stopLoad := context.WithCancel(context.Background())
	workers := launchWorkers(loadCtx, c, sources, domain, depth, o, stats, false)
	monitorCtx, stopMonitor := context.WithCancel(context.Background())
	var monitor sync.WaitGroup
	monitor.Add(1)
	go func() {
		defer monitor.Done()
		for monitorCtx.Err() == nil {
			snap, e := snapshot(monitorCtx, c, sources, false)
			if e == nil {
				e = requireTopology(snap, o.LaneDepth)
				if e != nil {
					stats.fail(e)
					stopLoad()
					return
				}
			}
			if e == nil {
				if e = applyObserved(sources, snap, o.Outputs); e != nil {
					stats.fail(e)
					fmt.Fprintln(os.Stderr, e)
					stopLoad()
					return
				}
			} else if monitorCtx.Err() == nil {
				fmt.Fprintf(os.Stderr, "nonce monitor: %v\n", e)
			}
			select {
			case <-monitorCtx.Done():
				return
			case <-time.After(time.Second):
			}
		}
	}()
	interrupted := false
	select {
	case <-ctx.Done():
		interrupted = true
	case <-time.After(o.Warmup):
	}
	if !interrupted {
		r.Start, err = snapshot(ctx, c, sources, true)
		if err == nil {
			err = requireTopology(r.Start, o.LaneDepth)
		}
	}
	if err == nil && !interrupted {
		fmt.Fprintf(os.Stderr, "measurement started at masterchain %d\n", r.Start.Masterchain.Seqno)
		remaining := time.Until(r.Start.CapturedAt.Add(o.Duration))
		if remaining < 0 {
			remaining = 0
		}
		select {
		case <-ctx.Done():
			interrupted = true
		case <-time.After(remaining):
		}
		if !interrupted {
			r.End, err = snapshot(ctx, c, sources, true)
			if err == nil {
				err = requireTopology(r.End, o.LaneDepth)
			}
		}
	}
	stopLoad()
	workers.Wait()
	stopMonitor()
	monitor.Wait()
	fmt.Fprintln(os.Stderr, "offers stopped; reconciling exact outstanding native requests")
	drainCtx, stopDrain := context.WithTimeout(context.Background(), o.DrainTimeout)
	defer stopDrain()
	drainWorkers := launchWorkers(drainCtx, c, sources, domain, depth, o, stats, true)
	var finalErr error
	var previous *nativerpc.Snapshot
	stable := 0
	for drainCtx.Err() == nil {
		snap, e := snapshot(drainCtx, c, sources, true)
		if e == nil {
			e = requireTopology(snap, o.LaneDepth)
		}
		if e != nil {
			finalErr = e
			time.Sleep(time.Second)
			continue
		}
		if e = applyObserved(sources, snap, o.Outputs); e != nil {
			finalErr = e
			break
		}
		all := true
		for _, s := range sources {
			s.Lock()
			all = all && s.observed == s.issued
			s.Unlock()
		}
		if all {
			if previous != nil && snap.Masterchain.Seqno > previous.Masterchain.Seqno {
				_, n, _, ae := auditInterval(previous, snap, sources, o)
				if ae != nil {
					finalErr = ae
					break
				}
				if n == 0 {
					stable++
				} else {
					stable = 0
				}
			}
			previous = snap
			if stable >= 2 {
				r.Final = snap
				r.Drained = true
				break
			}
		} else {
			stable = 0
			previous = nil
		}
		select {
		case <-drainCtx.Done():
		case <-time.After(time.Second):
		}
	}
	stopDrain()
	drainWorkers.Wait()
	r.Counts = stats.snapshot(o.Outputs)
	if err != nil {
		return r, err
	}
	if interrupted {
		return r, errors.New("measurement interrupted; drain attempted")
	}
	if failure := stats.failure(); failure != nil {
		return r, fmt.Errorf("native sender/monitor failed: %w", failure)
	}
	if !r.Drained {
		if finalErr != nil {
			return r, fmt.Errorf("drain failed: %w", finalErr)
		}
		return r, errors.New("native requests did not drain before deadline")
	}
	r.Audit, r.TotalPayments, r.TotalParents, err = auditInterval(r.Initial, r.Final, sources, o)
	if err != nil {
		return r, err
	}
	if r.TotalPayments != r.Counts.OfferedPayments || r.TotalParents != r.Counts.OfferedParents {
		return r, errors.New("final canonical nonce interval differs from exact offered cohort")
	}
	_, r.MeasuredPayments, r.MeasuredParents, err = auditInterval(r.Start, r.End, sources, o)
	if err != nil {
		return r, err
	}
	r.WallSeconds = r.End.CapturedAt.Sub(r.Start.CapturedAt).Seconds()
	r.ChainSeconds = int64(r.End.ChainTime) - int64(r.Start.ChainTime)
	if r.MeasuredPayments == 0 || r.WallSeconds <= 0 || r.ChainSeconds <= 0 {
		return r, errors.New("no advancing measured native payment interval")
	}
	r.PaymentTPS = float64(r.MeasuredPayments) / r.WallSeconds
	r.ParentTPS = float64(r.MeasuredParents) / r.WallSeconds
	r.ChainTPS = float64(r.MeasuredPayments) / float64(r.ChainSeconds)
	r.Valid = true
	fmt.Fprintf(os.Stderr, "verified %.2f payments/s, %.2f completed signed parents/s; TON internal message cells=0\n", r.PaymentTPS, r.ParentTPS)
	return r, nil
}

func main() {
	if len(os.Args) < 2 || os.Args[1] != "run" {
		fmt.Fprintln(os.Stderr, "usage: nativespam run --config FILE --wallet-dir DIR [options]")
		os.Exit(2)
	}
	f := flag.NewFlagSet("run", flag.ExitOnError)
	o := options{}
	f.StringVar(&o.Config, "config", "", "liteserver configuration")
	f.StringVar(&o.WalletDir, "wallet-dir", "", "funded native source-N.pk/pub and dest-N.pub directory")
	f.StringVar(&o.Output, "output", "", "JSON results path; stdout when empty")
	f.IntVar(&o.Sources, "sources", 1024, "number of disjoint funded source/destination pairs")
	f.IntVar(&o.LaneDepth, "lane-depth", 2, "required fixed lane depth (1..3); refuses incomplete genesis splits")
	f.IntVar(&o.Outputs, "outputs", 16, "logical payments per NTRN signed parent (1..16)")
	f.IntVar(&o.Workers, "workers", 8, "independent signing/submission workers")
	f.IntVar(&o.BatchSize, "batch-size", 64, "signed parents per sendMessageBatch RPC")
	f.IntVar(&o.Window, "window", 64, "maximum outstanding signed parents per source (<=4096 logical nonces)")
	f.Uint64Var(&o.Amount, "amount", 1, "nanograms paid per output")
	f.Uint64Var(&o.Fee, "fee", 0, "protocol fee in nanograms per output")
	f.Float64Var(&o.Rate, "rate", 0, "logical attempt rate ceiling including retries; 0 unrestricted")
	f.DurationVar(&o.Duration, "duration", 120*time.Second, "measurement interval")
	f.DurationVar(&o.Warmup, "warmup", 10*time.Second, "load before first measurement anchor")
	f.DurationVar(&o.DrainTimeout, "drain-timeout", 120*time.Second, "bounded final reconciliation")
	f.Parse(os.Args[2:])
	if o.Config == "" || o.WalletDir == "" || o.Sources < 1 || o.Sources > 65536 || o.LaneDepth < 1 || o.LaneDepth > 3 || o.Outputs < 1 || o.Outputs > 16 || o.Workers < 1 || o.Workers > o.Sources || o.BatchSize < 1 || o.BatchSize > 1024 || o.Window < 1 || o.Window > 4096/o.Outputs || o.Amount == 0 || o.Fee > math.MaxUint64-o.Amount || uint64(o.Outputs) > math.MaxUint64/(o.Amount+o.Fee) || o.Rate < 0 || math.IsNaN(o.Rate) || math.IsInf(o.Rate, 0) || o.Duration <= 0 || o.Warmup < 0 || o.DrainTimeout <= 0 || o.Duration > 550*time.Second || o.Warmup > 550*time.Second || o.DrainTimeout > 550*time.Second || o.Duration+o.Warmup+o.DrainTimeout > 550*time.Second {
		fmt.Fprintln(os.Stderr, "invalid native benchmark options; total run bounds must be <=550s")
		os.Exit(2)
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	r, err := run(ctx, o)
	data, e := json.MarshalIndent(r, "", "  ")
	if e != nil {
		fmt.Fprintln(os.Stderr, e)
		os.Exit(1)
	}
	data = append(data, '\n')
	if o.Output == "" {
		_, e = os.Stdout.Write(data)
	} else {
		e = os.WriteFile(o.Output, data, 0644)
	}
	if e != nil {
		fmt.Fprintln(os.Stderr, e)
		os.Exit(1)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
