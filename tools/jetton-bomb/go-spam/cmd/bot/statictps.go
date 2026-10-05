package main

import (
	"context"
	"fmt"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/walletv5"
)

// StaticTps opcodes mirror wrappers/StaticTps.ts.
const (
	OpStaticTpsRun  = 0x5ce7c1d2
	OpStaticTpsStop = 0x07c32b3f
)

// V5R1 default send mode = PAY_GAS_SEPARATELY (1) | IGNORE_ERRORS (2).
// IGNORE_ERRORS is required so seqno still advances if the inner action fails;
// without it the whole external is rejected and the wallet stalls.
const sendModePayGasSeparately = 1 | 2

// internalMessage builds an InternalMessage with the given destination, value (in nano TON),
// bounce flag, and body.
func internalMessage(dst *address.Address, valueNano tlb.Coins, bounce bool, body *cell.Cell) *tlb.InternalMessage {
	return &tlb.InternalMessage{
		IHRDisabled: true,
		Bounce:      bounce,
		DstAddr:     dst,
		Amount:      valueNano,
		Body:        body,
	}
}

// sendStaticTpsRun sends an internal message to one shard with op=run, tps payload.
func (r *Runtime) sendStaticTpsRun(ctx context.Context, shardIdx int, tps int) error {
	if shardIdx < 0 || shardIdx >= len(r.shardAddrs) {
		return fmt.Errorf("shard index %d out of range", shardIdx)
	}
	value, err := tlb.FromTON(RunAmountPerShardTON)
	if err != nil {
		return err
	}
	body := cell.BeginCell().
		MustStoreUInt(OpStaticTpsRun, 32).
		MustStoreUInt(uint64(tps), 32).
		EndCell()
	msg := internalMessage(r.shardAddrs[shardIdx], value, true, body)
	return r.wallet.SendMessages(ctx, r.Backend(), 0, []walletv5.Message{
		{Mode: sendModePayGasSeparately, Internal: msg},
	})
}

// sendStaticTpsStop sends an internal message to one shard with op=stop.
func (r *Runtime) sendStaticTpsStop(ctx context.Context, shardIdx int) error {
	if shardIdx < 0 || shardIdx >= len(r.shardAddrs) {
		return fmt.Errorf("shard index %d out of range", shardIdx)
	}
	value, err := tlb.FromTON(StopFeePerShardTON)
	if err != nil {
		return err
	}
	body := cell.BeginCell().
		MustStoreUInt(OpStaticTpsStop, 32).
		EndCell()
	msg := internalMessage(r.shardAddrs[shardIdx], value, true, body)
	return r.wallet.SendMessages(ctx, r.Backend(), 0, []walletv5.Message{
		{Mode: sendModePayGasSeparately, Internal: msg},
	})
}

// sendStaticTpsRunAll sends a single batched external with one internal per shard, all op=run.
func (r *Runtime) sendStaticTpsRunAll(ctx context.Context, tps int) error {
	value, err := tlb.FromTON(RunAmountPerShardTON)
	if err != nil {
		return err
	}
	body := cell.BeginCell().
		MustStoreUInt(OpStaticTpsRun, 32).
		MustStoreUInt(uint64(tps), 32).
		EndCell()
	msgs := make([]walletv5.Message, 0, len(r.shardAddrs))
	for _, s := range r.shardAddrs {
		msgs = append(msgs, walletv5.Message{
			Mode:     sendModePayGasSeparately,
			Internal: internalMessage(s, value, true, body),
		})
	}
	return r.wallet.SendMessages(ctx, r.Backend(), 0, msgs)
}

// sendStaticTpsStopAll batches stop messages to all shards.
func (r *Runtime) sendStaticTpsStopAll(ctx context.Context) error {
	value, err := tlb.FromTON(StopFeePerShardTON)
	if err != nil {
		return err
	}
	body := cell.BeginCell().
		MustStoreUInt(OpStaticTpsStop, 32).
		EndCell()
	msgs := make([]walletv5.Message, 0, len(r.shardAddrs))
	for _, s := range r.shardAddrs {
		msgs = append(msgs, walletv5.Message{
			Mode:     sendModePayGasSeparately,
			Internal: internalMessage(s, value, true, body),
		})
	}
	return r.wallet.SendMessages(ctx, r.Backend(), 0, msgs)
}
