package highload

import (
	"fmt"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

const (
	BatchInterval      = 500 * time.Millisecond
	MaxBatchMessages   = 254       // one of TVM's 255 actions is reserved for restoring the highload code
	MaxBatchBytes      = 60 * 1024 // full external BoC, below the node's 65535-byte limit
	BatchQueueLimit    = 4 * MaxBatchMessages
	BatchValidity      = 120 * time.Second
	batchTransferOp    = 0xae42e5a4
	batchSelfValueNano = 10_000_000 // funds self-message compute, then credits the same highload
)

// RequestTimeout allows a seqno fetch, queueing, and two independently timed batch RPC attempts.
func RequestTimeout(rpcTimeout time.Duration) time.Duration {
	return 3*rpcTimeout + 4*BatchInterval
}

// BuildBatchExternal always uses a self-message, including for a single relay.
func (w *HighloadWallet) BuildBatchExternal(messages []*cell.Cell, createdAt uint32) (*cell.Cell, error) {
	queryID := w.nextQueryID()
	msg, err := w.batchMessage(messages, queryID)
	if err != nil {
		return nil, err
	}
	ext, err := w.buildRelayExternal(msg, SendModeRelay, createdAt, queryID)
	if err != nil {
		return nil, err
	}
	if size := len(w.externalWireCell(ext).ToBOC()); size > MaxBatchBytes {
		return nil, fmt.Errorf("highload: batch external is %d bytes (limit %d)", size, MaxBatchBytes)
	}
	return ext, nil
}

func (w *HighloadWallet) batchMessage(messages []*cell.Cell, queryID uint32) (*cell.Cell, error) {
	if len(messages) == 0 || len(messages) > MaxBatchMessages {
		return nil, fmt.Errorf("highload: batch needs 1..%d messages, got %d", MaxBatchMessages, len(messages))
	}
	list := cell.BeginCell().EndCell()
	for _, msg := range messages {
		if msg == nil {
			return nil, errNilMessage
		}
		list = cell.BeginCell().MustStoreRef(list).
			MustStoreUInt(walletspam.ActionSendMsgOp, 32).
			MustStoreUInt(uint64(SendModeRelay), 8).MustStoreRef(msg).EndCell()
	}
	body := cell.BeginCell().MustStoreUInt(batchTransferOp, 32).
		MustStoreUInt(uint64(queryID), 64).MustStoreRef(list).EndCell()
	return tlb.ToCell(&tlb.InternalMessage{
		IHRDisabled: true, Bounce: false, DstAddr: w.Address(),
		Amount: tlb.FromNanoTONU(batchSelfValueNano), Body: body,
	})
}

// Match backend.buildExtMsgBoc, including its reference-stored body.
func (w *HighloadWallet) externalWireCell(body *cell.Cell) *cell.Cell {
	return cell.BeginCell().MustStoreUInt(0b10, 2).MustStoreUInt(0, 2).
		MustStoreAddr(w.Address()).MustStoreCoins(0).MustStoreUInt(0, 1).
		MustStoreUInt(1, 1).MustStoreRef(body).EndCell()
}

// Size estimates do not consume query IDs or signatures. Their fixed-width fields match the real BoC.
func (w *HighloadWallet) batchSize(messages []*cell.Cell) (int, error) {
	msg, err := w.batchMessage(messages, 0)
	if err != nil {
		return 0, err
	}
	payload := cell.BeginCell().MustStoreUInt(uint64(w.subwalletID), 32).MustStoreRef(msg).
		MustStoreUInt(uint64(SendModeRelay), 8).MustStoreUInt(0, 23).
		MustStoreUInt(0, 64).MustStoreUInt(uint64(w.timeout), 22).EndCell()
	body := cell.BeginCell().MustStoreSlice(make([]byte, 64), 512).MustStoreRef(payload).EndCell()
	return len(w.externalWireCell(body).ToBOC()), nil
}
