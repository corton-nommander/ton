// internalspam runs a bounded benchmark using jetton-bomb's original StaticTps
// self-message contract. Counter and receipt counts are anchored to the same
// masterchain account snapshots; local liteserver state is trusted explicitly.
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
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/sidechain"
	"github.com/victor/jetton-bomb-go-spam/pkg/statictps"
	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/liteclient"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/ton"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

type options struct {
	phase, config, code, owner, key, walletVersion, output             string
	workchain                                                          int
	n, target, nonce, subwallet                                        uint
	duration, warmup, poll, controlTimeout, drainTimeout, auditTimeout time.Duration
	funding                                                            string
	maxAudit                                                           uint64
}
type contract struct {
	addr *address.Address
	init *tlb.StateInit
}
type accountSnapshot struct {
	Address  string          `json:"address"`
	State    statictps.State `json:"state"`
	LastLT   uint64          `json:"last_transaction_lt"`
	LastHash string          `json:"last_transaction_hash"`
	Balance  string          `json:"balance_nano"`
}
type snapshot struct {
	Masterchain *ton.BlockIDExt   `json:"masterchain"`
	GenUtime    uint32            `json:"gen_utime"`
	Captured    time.Time         `json:"captured_at"`
	Accounts    []accountSnapshot `json:"accounts"`
}
type result struct {
	Schema                  string            `json:"schema"`
	Valid                   bool              `json:"valid"`
	Error                   string            `json:"error,omitempty"`
	Protocol                *sidechain.Report `json:"protocol,omitempty"`
	CodeHash                string            `json:"code_hash"`
	Workchain               int               `json:"workchain"`
	Contracts               uint              `json:"contracts"`
	TargetPerContract       uint              `json:"requested_target_tps_per_contract"`
	ContractCapPer30Seconds uint              `json:"contract_work_cap_per_30_seconds"`
	Measurement             string            `json:"measurement"`
	Start                   *snapshot         `json:"start,omitempty"`
	End                     *snapshot         `json:"end,omitempty"`
	Drain                   *snapshot         `json:"drain,omitempty"`
	ChainSeconds            uint32            `json:"canonical_window_seconds"`
	WallSeconds             float64           `json:"wall_window_seconds"`
	WorkTransactions        uint64            `json:"successful_work_counter_delta"`
	WorkTPS                 float64           `json:"successful_work_tps"`
	WorkWallTPS             float64           `json:"successful_work_wall_tps"`
	Receipts                statictps.Counts  `json:"receipts"`
	InternalTPS             float64           `json:"internal_transaction_tps"`
	InternalWallTPS         float64           `json:"internal_transaction_wall_tps"`
	AuditComplete           bool              `json:"audit_complete"`
	Drained                 bool              `json:"drained"`
	ProofPolicy             string            `json:"proof_policy"`
	CleanupError            string            `json:"cleanup_error,omitempty"`
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "internalspam:", err)
		if errors.Is(err, sidechain.ErrUnsupportedTVM) {
			os.Exit(sidechain.UnsupportedExitCode)
		}
		os.Exit(1)
	}
}

func parse(args []string) (options, error) {
	o := options{}
	if len(args) == 0 {
		return o, fmt.Errorf("usage: internalspam <plan|run|status|stop> --ls-config FILE --code FILE --owner WC:HEX --workchain -1 [flags]")
	}
	o.phase = args[0]
	if o.phase != "plan" && o.phase != "run" && o.phase != "status" && o.phase != "stop" {
		return o, fmt.Errorf("unknown phase %q", o.phase)
	}
	f := flag.NewFlagSet("internalspam", flag.ContinueOnError)
	f.StringVar(&o.config, "ls-config", "", "local TON global config JSON (required)")
	f.StringVar(&o.code, "code", "build/StaticTps.compiled.json", "original StaticTps compiled JSON or raw code BOC")
	f.StringVar(&o.owner, "owner", "", "raw/friendly control wallet address (required)")
	f.StringVar(&o.key, "owner-key", "", "raw 32-byte control wallet ed25519 seed file")
	f.StringVar(&o.walletVersion, "wallet-version", "v3", "control wallet format: v3 or v1")
	f.UintVar(&o.subwallet, "subwallet-id", 42, "V3 control wallet subwallet id")
	f.IntVar(&o.workchain, "workchain", -99, "target workchain: 0 or -1 (required)")
	f.UintVar(&o.n, "contracts", 1, "number of independent self-message contracts (1..256)")
	f.UintVar(&o.target, "target-tps", 3000, "requested work TPS per contract (actual original contract cap is 100000/30)")
	f.UintVar(&o.nonce, "nonce", 1, "first address nonce; reuse with status/stop")
	f.DurationVar(&o.duration, "duration", 30*time.Second, "measurement wall duration")
	f.DurationVar(&o.warmup, "warmup", 5*time.Second, "warmup after all starts confirmed")
	f.DurationVar(&o.poll, "poll", time.Second, "canonical snapshot polling interval")
	f.DurationVar(&o.controlTimeout, "control-timeout", 60*time.Second, "timeout per control external confirmation")
	f.DurationVar(&o.drainTimeout, "drain-timeout", 60*time.Second, "timeout for stopped contracts and two unchanged advancing anchors")
	f.DurationVar(&o.auditTimeout, "audit-timeout", 3*time.Minute, "bounded account-history receipt audit timeout")
	f.Uint64Var(&o.maxAudit, "max-audit-tx", 2000000, "maximum receipts audited; reaching cap invalidates run")
	f.StringVar(&o.funding, "funding", "1000", "TON/GRAM initially attached per contract")
	f.StringVar(&o.output, "output", "internalspam-result.json", "result JSON (run/status) or artifact directory (plan)")
	if err := f.Parse(args[1:]); err != nil {
		return o, err
	}
	if f.NArg() != 0 {
		return o, fmt.Errorf("unexpected arguments")
	}
	if o.config == "" || o.owner == "" {
		return o, fmt.Errorf("--ls-config and --owner are required")
	}
	if o.workchain != 0 && o.workchain != -1 {
		return o, fmt.Errorf("--workchain must explicitly be 0 or -1")
	}
	if o.n < 1 || o.n > 256 || uint64(o.nonce)+uint64(o.n)-1 > math.MaxUint32 {
		return o, fmt.Errorf("contracts must be 1..256 and nonces must fit uint32")
	}
	if o.target < 1 || uint64(o.target) > math.MaxUint32 || uint64(o.subwallet) > math.MaxUint32 {
		return o, fmt.Errorf("target/subwallet must fit uint32, target must be positive")
	}
	if o.duration <= 0 || o.warmup < 0 || o.poll <= 0 || o.controlTimeout <= 0 || o.drainTimeout <= 0 || o.auditTimeout <= 0 || o.maxAudit < 1 {
		return o, fmt.Errorf("invalid duration or audit bound")
	}
	if o.walletVersion != "v1" && o.walletVersion != "v3" {
		return o, fmt.Errorf("--wallet-version must be v1 or v3")
	}
	funding, err := tlb.FromTON(o.funding)
	if err != nil {
		return o, fmt.Errorf("funding: %w", err)
	}
	if funding.Nano().Sign() <= 0 {
		return o, fmt.Errorf("funding must be positive")
	}
	return o, nil
}

func parseAddress(s string) (*address.Address, error) {
	if strings.Contains(s, ":") {
		return address.ParseRawAddr(s)
	}
	return address.ParseAddr(s)
}
func makeContracts(o options, owner *address.Address, code *cell.Cell) []contract {
	list := make([]contract, o.n)
	for i := range list {
		init := &tlb.StateInit{Code: code, Data: statictps.Data(owner, uint32(o.nonce+uint(i)))}
		list[i] = contract{init.CalcAddress(o.workchain), init}
	}
	return list
}
func writeJSON(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(data, '\n'), 0600)
}

func run(args []string) (runErr error) {
	o, err := parse(args)
	if err != nil {
		return err
	}
	owner, err := parseAddress(o.owner)
	if err != nil {
		return err
	}
	code, err := statictps.LoadCode(o.code)
	if err != nil {
		return fmt.Errorf("code: %w", err)
	}
	cs := makeContracts(o, owner, code)
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	preCtx, preCancel := context.WithTimeout(ctx, 20*time.Second)
	report, err := sidechain.RequireTVM(preCtx, o.config, int32(o.workchain))
	preCancel()
	if err != nil {
		return err
	}
	if o.phase == "plan" {
		return plan(o, cs, code, report)
	}
	if o.phase == "run" || o.phase == "stop" {
		preCtx, preCancel = context.WithTimeout(ctx, 20*time.Second)
		_, err = sidechain.RequireTVM(preCtx, o.config, owner.Workchain())
		preCancel()
		if err != nil {
			return fmt.Errorf("control wallet: %w", err)
		}
	}
	pool := liteclient.NewConnectionPool()
	if err := pool.AddConnectionsFromConfigFile(o.config); err != nil {
		return err
	}
	defer pool.Stop()
	api := ton.NewAPIClient(pool, ton.ProofCheckPolicyUnsafe).WithRetry(2)
	r := result{Schema: "jetton-bomb-internalspam-v1", Protocol: report, CodeHash: hex.EncodeToString(code.Hash()), Workchain: o.workchain, Contracts: o.n, TargetPerContract: o.target, ContractCapPer30Seconds: 100000, Measurement: "exact account receipt and successful self-work counter deltas between canonical masterchain account anchors; deployment, warmup and stop/drain excluded", ProofPolicy: "trusted local liteserver masterchain account snapshots; receipt predecessor hashes verified by tonutils-go; no independent consensus proof verification"}
	defer func() {
		if runErr != nil {
			r.Error = runErr.Error()
		}
		if err := writeJSON(o.output, &r); err != nil {
			runErr = errors.Join(runErr, err)
		}
		data, _ := json.Marshal(&r)
		fmt.Println(string(data))
	}()
	if o.phase == "status" {
		s, err := capture(ctx, api, cs, code)
		r.End = s
		r.Valid = err == nil
		return err
	}
	seed, err := os.ReadFile(o.key)
	if err != nil {
		return fmt.Errorf("owner key: %w", err)
	}
	if len(seed) != ed25519.SeedSize {
		return fmt.Errorf("owner key must contain exactly 32 raw bytes")
	}
	key := ed25519.NewKeyFromSeed(seed)
	send := func(ctx context.Context, kind string) error { return control(ctx, api, o, owner, key, cs, kind) }
	stopAndDrain := func(ctx context.Context) error {
		if err := send(ctx, "stop"); err != nil {
			return err
		}
		s, err := drain(ctx, api, o, cs, code)
		r.Drain = s
		r.Drained = err == nil
		return err
	}
	if o.phase == "stop" {
		err = stopAndDrain(ctx)
		r.Valid = err == nil
		return err
	}
	started := false
	defer func() {
		if started && !r.Drained {
			cleanCtx, cleanCancel := context.WithTimeout(context.Background(), o.drainTimeout+o.controlTimeout*time.Duration((o.n+3)/4))
			defer cleanCancel()
			if err := stopAndDrain(cleanCtx); err != nil {
				r.CleanupError = err.Error()
				runErr = errors.Join(runErr, fmt.Errorf("cleanup: %w", err))
			}
		}
	}()
	if err := send(ctx, "deploy"); err != nil {
		return err
	}
	if _, err := waitContracts(ctx, api, o, cs, code, false); err != nil {
		return fmt.Errorf("deployed contract check: %w", err)
	}
	// Set before sending: an uncertain RPC error may still have started a subset.
	started = true
	if err := send(ctx, "start"); err != nil {
		return err
	}
	if _, err := waitContracts(ctx, api, o, cs, code, true); err != nil {
		return fmt.Errorf("start confirmation: %w", err)
	}
	if err := pause(ctx, o.warmup); err != nil {
		return err
	}
	r.Start, err = capture(ctx, api, cs, code)
	if err != nil {
		return err
	}
	for _, a := range r.Start.Accounts {
		if !a.State.Running {
			return fmt.Errorf("contract %s did not start", a.Address)
		}
	}
	deadline := time.Now().Add(o.duration)
	for time.Now().Before(deadline) {
		remaining := time.Until(deadline)
		step := o.poll
		if remaining < step {
			step = remaining
		}
		if err := pause(ctx, step); err != nil {
			return err
		}
	}
	r.End, err = capture(ctx, api, cs, code)
	if err != nil {
		return err
	}
	if r.End.GenUtime <= r.Start.GenUtime {
		return fmt.Errorf("canonical block timestamp did not advance; no valid TPS denominator")
	}
	r.ChainSeconds = r.End.GenUtime - r.Start.GenUtime
	r.WallSeconds = r.End.Captured.Sub(r.Start.Captured).Seconds()
	for i, a := range r.End.Accounts {
		if a.State.TotalTx < r.Start.Accounts[i].State.TotalTx {
			return fmt.Errorf("work counter moved backwards")
		}
		r.WorkTransactions += a.State.TotalTx - r.Start.Accounts[i].State.TotalTx
	}
	r.WorkTPS = float64(r.WorkTransactions) / float64(r.ChainSeconds)
	r.WorkWallTPS = float64(r.WorkTransactions) / r.WallSeconds
	if err := stopAndDrain(ctx); err != nil {
		return err
	}
	auditCtx, auditCancel := context.WithTimeout(ctx, o.auditTimeout)
	defer auditCancel()
	r.Receipts, err = audit(auditCtx, api, cs, r.Start, r.End, o.maxAudit)
	if err != nil {
		return fmt.Errorf("receipt audit: %w", err)
	}
	r.AuditComplete = true
	r.InternalTPS = float64(r.Receipts.Internal) / float64(r.ChainSeconds)
	r.InternalWallTPS = float64(r.Receipts.Internal) / r.WallSeconds
	if r.WorkTransactions > r.Receipts.WorkReceipts-r.Receipts.WorkAborted {
		return fmt.Errorf("counter delta exceeds successful self-work receipts")
	}
	r.Valid = r.WorkTransactions > 0 && r.Drained && r.AuditComplete
	if !r.Valid {
		return fmt.Errorf("no successful self-work observed")
	}
	return nil
}

func pause(ctx context.Context, d time.Duration) error {
	timer := time.NewTimer(d)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return nil
	}
}

// A control wallet nonce confirms the external, not its downstream internal
// messages. Wait for contract account state at a canonical anchor separately.
func waitContracts(ctx context.Context, api ton.APIClientWrapped, o options, cs []contract, code *cell.Cell, running bool) (*snapshot, error) {
	waitCtx, cancel := context.WithTimeout(ctx, o.controlTimeout)
	defer cancel()
	var lastErr error
	for {
		s, err := capture(waitCtx, api, cs, code)
		if err == nil {
			ready := true
			for _, a := range s.Accounts {
				if running && !a.State.Running {
					ready = false
					lastErr = fmt.Errorf("contract %s has not entered running state", a.Address)
				}
			}
			if ready {
				return s, nil
			}
		} else {
			lastErr = err
		}
		if err := pause(waitCtx, o.poll); err != nil {
			return nil, fmt.Errorf("contract readiness: %w (last observation: %v)", err, lastErr)
		}
	}
}

func capture(ctx context.Context, api ton.APIClientWrapped, cs []contract, code *cell.Cell) (*snapshot, error) {
	callCtx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	mc, err := api.GetMasterchainInfo(callCtx)
	if err != nil {
		return nil, err
	}
	hdr, err := api.GetBlockHeader(callCtx, mc)
	if err != nil {
		return nil, err
	}
	s := &snapshot{Masterchain: mc, GenUtime: hdr.GenUtime, Captured: time.Now().UTC()}
	for _, c := range cs {
		acc, err := api.GetAccount(callCtx, mc, c.addr)
		if err != nil {
			return nil, err
		}
		if acc == nil || !acc.IsActive || acc.Code == nil || acc.Data == nil || acc.State == nil {
			return nil, fmt.Errorf("contract %s is not active", c.addr.StringRaw())
		}
		if !bytes.Equal(acc.Code.Hash(), code.Hash()) {
			return nil, fmt.Errorf("contract %s code hash mismatch", c.addr.StringRaw())
		}
		state, err := statictps.ParseState(acc.Data)
		if err != nil {
			return nil, fmt.Errorf("contract %s data: %w", c.addr.StringRaw(), err)
		}
		s.Accounts = append(s.Accounts, accountSnapshot{c.addr.StringRaw(), state, acc.LastTxLT, hex.EncodeToString(acc.LastTxHash), acc.State.Balance.Nano().String()})
	}
	return s, nil
}

func ownerSeqno(ctx context.Context, api ton.APIClientWrapped, o options, owner *address.Address, key ed25519.PrivateKey) (uint32, error) {
	mc, err := api.GetMasterchainInfo(ctx)
	if err != nil {
		return 0, err
	}
	acc, err := api.GetAccount(ctx, mc, owner)
	if err != nil {
		return 0, err
	}
	if acc == nil || !acc.IsActive || acc.Data == nil {
		return 0, fmt.Errorf("control wallet is not active")
	}
	s := acc.Data.BeginParse()
	seq, err := s.LoadUInt(32)
	if err != nil {
		return 0, err
	}
	if o.walletVersion == "v3" {
		subwallet, err := s.LoadUInt(32)
		if err != nil {
			return 0, err
		}
		if subwallet != uint64(o.subwallet) {
			return 0, fmt.Errorf("control wallet subwallet id mismatch")
		}
	}
	pub, err := s.LoadSlice(256)
	if err != nil {
		return 0, err
	}
	if !bytes.Equal(pub, key.Public().(ed25519.PublicKey)) {
		return 0, fmt.Errorf("control wallet public key mismatch (wrong wallet version or seed)")
	}
	if s.BitsLeft() != 0 || s.RefsNum() != 0 {
		return 0, fmt.Errorf("control wallet data does not match %s layout", o.walletVersion)
	}
	return uint32(seq), nil
}

func control(ctx context.Context, api ton.APIClientWrapped, o options, owner *address.Address, key ed25519.PrivateKey, cs []contract, kind string) error {
	for offset := 0; offset < len(cs); offset += 4 {
		callCtx, cancel := context.WithTimeout(ctx, o.controlTimeout)
		seq, err := ownerSeqno(callCtx, api, o, owner, key)
		if err != nil {
			cancel()
			return err
		}
		var messages []*cell.Cell
		end := offset + 4
		if end > len(cs) {
			end = len(cs)
		}
		for _, c := range cs[offset:end] {
			value := tlb.MustFromTON("0.1")
			body := cell.BeginCell().EndCell()
			var init *tlb.StateInit
			switch kind {
			case "deploy":
				value = tlb.MustFromTON(o.funding)
				init = c.init
			case "start":
				body = statictps.RunBody(uint32(o.target))
			case "stop":
				body = statictps.StopBody()
			default:
				cancel()
				return fmt.Errorf("invalid control action")
			}
			msg, err := tlb.ToCell(&tlb.InternalMessage{IHRDisabled: true, Bounce: kind != "deploy", DstAddr: c.addr, Amount: value, Body: body, StateInit: init})
			if err != nil {
				cancel()
				return err
			}
			messages = append(messages, msg)
		}
		var body *cell.Cell
		if o.walletVersion == "v3" {
			body, err = statictps.V3External(seq, uint32(o.subwallet), uint32(time.Now().Unix()+int64(o.controlTimeout.Seconds())+60), key, messages)
		} else {
			body, err = statictps.V1External(seq, key, messages)
		}
		if err != nil {
			cancel()
			return err
		}
		if err = api.SendExternalMessage(callCtx, &tlb.ExternalMessage{DstAddr: owner, Body: body}); err != nil {
			cancel()
			return fmt.Errorf("%s batch send: %w", kind, err)
		}
		for {
			observed, err := ownerSeqno(callCtx, api, o, owner, key)
			if err != nil {
				cancel()
				return fmt.Errorf("%s confirmation: %w", kind, err)
			}
			if observed > seq {
				break
			}
			if err := pause(callCtx, o.poll); err != nil {
				cancel()
				return fmt.Errorf("%s confirmation: %w", kind, err)
			}
		}
		cancel()
		fmt.Fprintf(os.Stderr, "%s control confirmed for contracts %d..%d\n", kind, offset, end-1)
	}
	return nil
}

func drain(ctx context.Context, api ton.APIClientWrapped, o options, cs []contract, code *cell.Cell) (*snapshot, error) {
	drainCtx, cancel := context.WithTimeout(ctx, o.drainTimeout)
	defer cancel()
	var prev *snapshot
	stable := 0
	for {
		s, err := capture(drainCtx, api, cs, code)
		if err != nil {
			return s, err
		}
		allStopped := true
		for _, a := range s.Accounts {
			if a.State.Running {
				allStopped = false
			}
		}
		if allStopped && prev != nil && s.Masterchain.SeqNo > prev.Masterchain.SeqNo {
			unchanged := true
			for i, a := range s.Accounts {
				if a.LastLT != prev.Accounts[i].LastLT {
					unchanged = false
				}
			}
			if unchanged {
				stable++
			} else {
				stable = 0
			}
			if stable >= 2 {
				return s, nil
			}
		}
		if prev == nil || s.Masterchain.SeqNo > prev.Masterchain.SeqNo {
			prev = s
		}
		if err := pause(drainCtx, o.poll); err != nil {
			return s, fmt.Errorf("stop/drain not confirmed: %w", err)
		}
	}
}

func audit(ctx context.Context, api ton.APIClientWrapped, cs []contract, start, end *snapshot, maxTx uint64) (statictps.Counts, error) {
	counts := statictps.Counts{}
	for i, c := range cs {
		from, to := start.Accounts[i], end.Accounts[i]
		if to.LastLT < from.LastLT {
			return counts, fmt.Errorf("account LT moved backwards")
		}
		lt := to.LastLT
		hash, err := hex.DecodeString(to.LastHash)
		if err != nil {
			return counts, err
		}
		for lt > from.LastLT {
			list, err := api.ListTransactions(ctx, c.addr, 256, lt, hash)
			if err != nil {
				return counts, err
			}
			if len(list) == 0 {
				return counts, fmt.Errorf("empty account history before start anchor")
			}
			newest := list[len(list)-1]
			if newest.LT != lt || !bytes.Equal(newest.Hash, hash) {
				return counts, fmt.Errorf("history page does not match requested account anchor")
			}
			for j := len(list) - 1; j >= 0; j-- {
				tx := list[j]
				if tx.LT <= from.LastLT {
					break
				}
				if counts.Transactions >= maxTx {
					return counts, fmt.Errorf("audit cap %d reached", maxTx)
				}
				if !bytes.Equal(tx.AccountAddr, c.addr.Data()) {
					return counts, fmt.Errorf("history account mismatch")
				}
				counts.Add(tx, c.addr)
			}
			oldest := list[0]
			if oldest.LT <= from.LastLT {
				var found bool
				for _, tx := range list {
					if tx.LT == from.LastLT {
						found = hex.EncodeToString(tx.Hash) == from.LastHash
						break
					}
				}
				if !found {
					return counts, fmt.Errorf("start anchor missing or forked in history")
				}
				lt = from.LastLT
				hash, _ = hex.DecodeString(from.LastHash)
				break
			}
			if oldest.PrevTxLT >= lt {
				return counts, fmt.Errorf("history did not move backwards")
			}
			lt = oldest.PrevTxLT
			hash = oldest.PrevTxHash
		}
		if lt != from.LastLT || hex.EncodeToString(hash) != from.LastHash {
			return counts, fmt.Errorf("history predecessor does not match start anchor")
		}
		fmt.Fprintf(os.Stderr, "receipt audit contract %d complete, total internal=%d work=%d aborted=%d\n", i, counts.Internal, counts.WorkReceipts, counts.Aborted)
	}
	return counts, nil
}

func plan(o options, cs []contract, code *cell.Cell, report *sidechain.Report) error {
	if err := os.MkdirAll(o.output, 0700); err != nil {
		return err
	}
	targets := make([]map[string]any, 0, len(cs))
	for i, c := range cs {
		init, err := tlb.ToCell(c.init)
		if err != nil {
			return err
		}
		prefix := fmt.Sprintf("contract-%03d", i)
		for name, boc := range map[string]*cell.Cell{"init": init, "deploy-body": cell.BeginCell().EndCell(), "run-body": statictps.RunBody(uint32(o.target)), "stop-body": statictps.StopBody()} {
			if err := os.WriteFile(filepath.Join(o.output, prefix+"-"+name+".boc"), boc.ToBOC(), 0600); err != nil {
				return err
			}
		}
		targets = append(targets, map[string]any{"address": c.addr.StringRaw(), "nonce": o.nonce + uint(i), "artifact_prefix": prefix, "funding": o.funding})
	}
	return writeJSON(filepath.Join(o.output, "plan.json"), map[string]any{"protocol": report, "workchain": o.workchain, "owner": o.owner, "code_hash": hex.EncodeToString(code.Hash()), "target_tps_per_contract": o.target, "original_cap_work_per_30_seconds": 100000, "contracts": targets})
}
