// Package statictps implements the original StaticTps self-message contract.
package statictps

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

const (
	RunOp  = 0x5ce7c1d2
	StopOp = 0x07c32b3f
	WorkOp = 0x10adbeef
)

type State struct {
	Owner       string `json:"owner"`
	Running     bool   `json:"running"`
	TargetTPS   uint32 `json:"target_tps"`
	WindowStart uint32 `json:"window_start"`
	WindowTx    uint64 `json:"window_transactions"`
	TotalTx     uint64 `json:"total_work_transactions"`
	Epoch       uint32 `json:"epoch"`
}

func Data(owner *address.Address, nonce uint32) *cell.Cell {
	return cell.BeginCell().MustStoreAddr(owner).MustStoreUInt(0, 1).
		MustStoreUInt(uint64(nonce), 32).MustStoreUInt(0, 32).
		MustStoreUInt(0, 64).MustStoreUInt(0, 64).MustStoreUInt(0, 32).EndCell()
}

func ParseState(data *cell.Cell) (State, error) {
	if data == nil {
		return State{}, fmt.Errorf("missing contract data")
	}
	s := data.BeginParse()
	owner, err := s.LoadAddr()
	if err != nil {
		return State{}, err
	}
	if owner == nil {
		return State{}, fmt.Errorf("owner is addr_none")
	}
	values := make([]uint64, 6)
	for i, width := range []uint{1, 32, 32, 64, 64, 32} {
		values[i], err = s.LoadUInt(width)
		if err != nil {
			return State{}, err
		}
	}
	if s.BitsLeft() != 0 || s.RefsNum() != 0 {
		return State{}, fmt.Errorf("unexpected StaticTps storage suffix")
	}
	return State{owner.StringRaw(), values[0] != 0, uint32(values[1]), uint32(values[2]), values[3], values[4], uint32(values[5])}, nil
}

func RunBody(target uint32) *cell.Cell {
	return cell.BeginCell().MustStoreUInt(RunOp, 32).MustStoreUInt(uint64(target), 32).EndCell()
}
func StopBody() *cell.Cell { return cell.BeginCell().MustStoreUInt(StopOp, 32).EndCell() }

// LoadCode accepts a Blueprint compiled JSON, an Acton build JSON, or a raw BOC.
func LoadCode(path string) (*cell.Cell, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(strings.TrimSpace(string(data))) > 0 && strings.HasPrefix(strings.TrimSpace(string(data)), "{") {
		var artifact struct {
			Hex string `json:"hex"`
			BOC string `json:"code_boc64"`
		}
		if err := json.Unmarshal(data, &artifact); err != nil {
			return nil, err
		}
		switch {
		case artifact.Hex != "":
			data, err = hex.DecodeString(artifact.Hex)
		case artifact.BOC != "":
			data, err = base64.StdEncoding.DecodeString(artifact.BOC)
		default:
			return nil, fmt.Errorf("compiled JSON has neither hex nor code_boc64")
		}
		if err != nil {
			return nil, err
		}
	}
	return cell.FromBOC(data)
}

// V1External matches crypto/smartcont/wallet.fif. It must only be used with
// the simple V1 control wallet; modern wallets have different signed layouts.
func V1External(seqno uint32, key ed25519.PrivateKey, messages []*cell.Cell) (*cell.Cell, error) {
	if len(key) != ed25519.PrivateKeySize {
		return nil, fmt.Errorf("invalid ed25519 private key length")
	}
	if len(messages) < 1 || len(messages) > 4 {
		return nil, fmt.Errorf("V1 control batch needs 1..4 messages")
	}
	b := cell.BeginCell().MustStoreUInt(uint64(seqno), 32)
	for _, msg := range messages {
		if msg == nil {
			return nil, fmt.Errorf("nil internal message")
		}
		b.MustStoreUInt(3, 8).MustStoreRef(msg)
	}
	signed := b.EndCell()
	return cell.BeginCell().MustStoreSlice(ed25519.Sign(key, signed.Hash()), 512).MustStoreBuilder(signed.ToBuilder()).EndCell(), nil
}

// V3External matches crypto/smartcont/wallet-v3.fif and the genesis main wallet.
func V3External(seqno, subwallet, validUntil uint32, key ed25519.PrivateKey, messages []*cell.Cell) (*cell.Cell, error) {
	if len(key) != ed25519.PrivateKeySize {
		return nil, fmt.Errorf("invalid ed25519 private key length")
	}
	if len(messages) < 1 || len(messages) > 4 {
		return nil, fmt.Errorf("V3 control batch needs 1..4 messages")
	}
	b := cell.BeginCell().MustStoreUInt(uint64(subwallet), 32).MustStoreUInt(uint64(validUntil), 32).MustStoreUInt(uint64(seqno), 32)
	for _, msg := range messages {
		if msg == nil {
			return nil, fmt.Errorf("nil internal message")
		}
		b.MustStoreUInt(3, 8).MustStoreRef(msg)
	}
	signed := b.EndCell()
	return cell.BeginCell().MustStoreSlice(ed25519.Sign(key, signed.Hash()), 512).MustStoreBuilder(signed.ToBuilder()).EndCell(), nil
}

type Counts struct {
	Transactions uint64 `json:"transactions"`
	Internal     uint64 `json:"internal_transactions"`
	External     uint64 `json:"external_transactions"`
	Aborted      uint64 `json:"aborted_transactions"`
	Bounced      uint64 `json:"bounced_internal_transactions"`
	WorkReceipts uint64 `json:"self_work_receipts"`
	WorkAborted  uint64 `json:"aborted_self_work_receipts"`
}

func (c *Counts) Add(tx *tlb.Transaction, account *address.Address) {
	c.Transactions++
	aborted := false
	switch d := tx.Description.(type) {
	case tlb.TransactionDescriptionOrdinary:
		aborted = d.Aborted
	case *tlb.TransactionDescriptionOrdinary:
		aborted = d.Aborted
	}
	if aborted {
		c.Aborted++
	}
	if tx.IO.In == nil {
		return
	}
	if tx.IO.In.MsgType != tlb.MsgTypeInternal {
		c.External++
		return
	}
	c.Internal++
	m := tx.IO.In.AsInternal()
	if m.Bounced {
		c.Bounced++
	}
	if !m.Bounced && m.SrcAddr != nil && m.SrcAddr.Equals(account) && m.Body != nil {
		op, err := m.Body.BeginParse().LoadUInt(32)
		if err == nil && op == WorkOp {
			c.WorkReceipts++
			if aborted {
				c.WorkAborted++
			}
		}
	}
}
