package backend

import (
	"context"
	"fmt"
	"math/big"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/liteclient"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/ton"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

// LSBackend wraps a tonutils-go LiteServer pool.
type LSBackend struct {
	api     ton.APIClientWrapped
	pool    *liteclient.ConnectionPool
	servers int
}

// NewLSFromConfigFile builds a Backend from a TON global config JSON file path.
func NewLSFromConfigFile(path string) (*LSBackend, int, error) {
	pool := liteclient.NewConnectionPool()
	if err := pool.AddConnectionsFromConfigFile(path); err != nil {
		return nil, 0, fmt.Errorf("ls config %s: %w", path, err)
	}
	api := ton.NewAPIClient(pool).WithRetry(0)
	count := numServersFromConfig(path)
	return &LSBackend{api: api, pool: pool, servers: count}, count, nil
}

// numServersFromConfig is best-effort: re-parses the file to count entries.
// Returns 0 on failure (Description() will then show "lite (?)").
func numServersFromConfig(path string) int {
	data, err := readConfigServers(path)
	if err != nil {
		return 0
	}
	return data
}

func (b *LSBackend) Kind() Kind { return KindLite }

func (b *LSBackend) Description() string {
	if b.servers > 0 {
		return fmt.Sprintf("lite (%d servers)", b.servers)
	}
	return "lite"
}

func (b *LSBackend) API() ton.APIClientWrapped { return b.api }

func (b *LSBackend) GetSeqno(ctx context.Context, addr *address.Address) (uint32, error) {
	block, err := b.api.CurrentMasterchainInfo(ctx)
	if err != nil {
		return 0, fmt.Errorf("ls master info: %w", err)
	}
	res, err := b.api.RunGetMethod(ctx, block, addr, "seqno")
	if err != nil {
		return 0, fmt.Errorf("ls run seqno: %w", err)
	}
	n, err := res.Int(0)
	if err != nil {
		return 0, fmt.Errorf("ls seqno result: %w", err)
	}
	return uint32(n.Uint64()), nil
}

func (b *LSBackend) GetBalance(ctx context.Context, addr *address.Address) (*big.Int, error) {
	block, err := b.api.CurrentMasterchainInfo(ctx)
	if err != nil {
		return nil, fmt.Errorf("ls master info: %w", err)
	}
	acc, err := b.api.GetAccount(ctx, block, addr)
	if err != nil {
		return nil, fmt.Errorf("ls get account: %w", err)
	}
	if acc == nil || !acc.IsActive || acc.State == nil {
		return big.NewInt(0), nil // uninitialized / non-existent => 0
	}
	return acc.State.Balance.Nano(), nil
}

func (b *LSBackend) SendExternalMessage(ctx context.Context, dst *address.Address, body *cell.Cell) error {
	ext := &tlb.ExternalMessage{DstAddr: dst, Body: body}
	return b.api.SendExternalMessage(ctx, ext)
}

func (b *LSBackend) SendExternalMessageWithInit(ctx context.Context, dst *address.Address, body *cell.Cell, stateInit *tlb.StateInit) error {
	ext := &tlb.ExternalMessage{DstAddr: dst, StateInit: stateInit, Body: body}
	return b.api.SendExternalMessage(ctx, ext)
}

func (b *LSBackend) RunGetMethod(ctx context.Context, addr *address.Address, method string, args ...any) ([]StackValue, error) {
	block, err := b.api.CurrentMasterchainInfo(ctx)
	if err != nil {
		return nil, err
	}
	res, err := b.api.RunGetMethod(ctx, block, addr, method, args...)
	if err != nil {
		return nil, err
	}
	return executionResultToStack(res), nil
}

func executionResultToStack(res *ton.ExecutionResult) []StackValue {
	tup := res.AsTuple()
	out := make([]StackValue, 0, len(tup))
	for _, v := range tup {
		switch t := v.(type) {
		case *cell.Cell:
			out = append(out, StackValue{Kind: "cell", Cell: t})
		case *cell.Slice:
			out = append(out, StackValue{Kind: "slice", Slice: t})
		default:
			// fallback: try to read as integer via the typed helpers
			if i, err := res.Int(uint(len(out))); err == nil {
				out = append(out, StackValue{Kind: "int", Int: i})
				continue
			}
			out = append(out, StackValue{Kind: "unknown"})
		}
	}
	return out
}
