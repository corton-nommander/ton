// Package backend defines the small TON RPC surface the bot needs.
// Two implementations: LiteServer (via tonutils-go) and Toncenter v3 HTTP.
package backend

import (
	"context"
	"errors"
	"math/big"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

// Kind discriminates the backend at runtime (mostly for status messages).
type Kind string

const (
	KindLite Kind = "lite"
	KindHTTP Kind = "http"
)

// StackValue is the result of a get-method call. We only support the few primitive types
// the bot needs (uint, slice for address, cell). Match the order of the contract's return stack.
type StackValue struct {
	Kind  string // "int" | "cell" | "slice"
	Int   *big.Int
	Cell  *cell.Cell
	Slice *cell.Slice
}

// AsUint helper.
func (v StackValue) AsUint() (uint64, error) {
	if v.Kind != "int" || v.Int == nil {
		return 0, errors.New("stack value is not int")
	}
	return v.Int.Uint64(), nil
}

// Backend is the runtime-swappable RPC interface used by the bot.
type Backend interface {
	Kind() Kind
	// GetSeqno reads contract method "seqno" — used for both V5R1 control wallet and WalletSpam.
	GetSeqno(ctx context.Context, addr *address.Address) (uint32, error)
	// GetBalance reads an account's GRAM balance in nanoTON. Uninitialized/non-existent accounts
	// return 0 (not an error). Used by the bot's /check_balance gate to skip drained pool wallets.
	GetBalance(ctx context.Context, addr *address.Address) (*big.Int, error)
	// SendExternalMessage posts a signed external message body to the given destination.
	SendExternalMessage(ctx context.Context, dst *address.Address, body *cell.Cell) error
	// SendExternalMessageWithInit posts an external that ALSO carries the destination's state_init,
	// deploying an uninitialized account in the same message. Used by pkg/deploy for the pool kickoff:
	// wallet0 is funded (uninit) then this external deploys it + runs its onExternalMessage.
	SendExternalMessageWithInit(ctx context.Context, dst *address.Address, body *cell.Cell, stateInit *tlb.StateInit) error
	// RunGetMethod invokes a get-method on a contract; returns the result stack.
	RunGetMethod(ctx context.Context, addr *address.Address, method string, args ...any) ([]StackValue, error)
	// Description is shown in user-facing replies (e.g. "lite (4 servers)" or "http https://toncenter.com/...").
	Description() string
}
