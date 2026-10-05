// Package fats drives "send jettons TO fat load-target accounts FROM the generated wallet-v5 wallets".
//
// The state generator (ton/benchmark/state-gen.cpp) places, in the zerostate: 2.5M wallet-v5 wallets
// each with its own prepaid jetton wallet, plus a few thousand "fat" accounts with a large storage dict.
// A jetton transfer W5 -> its own JW -> op::internal_transfer -> the fat's JW (auto-deployed on first
// send, init mode 0) -> op::transfer_notification -> the fat owner. The notification (forward_ton > 0)
// is what lands a transaction ON the fat, forcing the collator to walk its dict. The first bit of the
// forward payload is a mutate flag the fat reads to decide whether to bump its nonce (defeating the
// storage-stat cache) — see jetton-prepaid/contracts/Fat.tolk.
package fats

import (
	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/jetton"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletv5"
)

// w5SendMode = SEND_MODE_PAY_FEES_SEPARATELY (1) | SEND_MODE_IGNORE_ERRORS (2), matching the WalletSpam
// jetton path. Fees come out of the wallet, not the attached value, and a failed transfer never bounces.
const w5SendMode = 1 | 2

// WalletJettonAddress is the prepaid jetton-wallet address of an owner (re-exported from pkg/jetton so
// callers of this package don't need a second import).
func WalletJettonAddress(owner, minter *address.Address, jwCode *cell.Cell) *address.Address {
	return jetton.WalletAddress(owner, minter, jwCode)
}

// BuildW5JettonTransfer builds a wallet-v5-signed external carrying one op::transfer of 1 jetton from
// `sender`'s own (pre-deployed) prepaid jetton wallet to `recipientOwner`'s jetton wallet.
//
// initMode is the recipient internal_init_mode: 0 = full state_init (auto-deploys the recipient's jetton
// wallet — used for fats, whose jetton wallets aren't pre-deployed); 1 = bare, no state_init (the cheapest
// correct mode when the recipient's jetton wallet is already deployed — used for W5->W5, since every
// generated wallet's jetton wallet is in the zerostate). forwardTON>0 makes the recipient jetton wallet
// forward a transfer_notification to recipientOwner; mutate then sets the fat mutate flag in the forward
// payload (true => a non-empty ref-form payload whose leading Either bit is 1).
func BuildW5JettonTransfer(
	sender *walletv5.Wallet,
	seqno uint32,
	jwCode *cell.Cell,
	minter *address.Address,
	value tlb.Coins,
	initMode int32,
	recipientOwner *address.Address,
	forwardTON uint64,
	mutate bool,
	sendExcesses bool,
	validUntil uint32,
) (*cell.Cell, error) {
	senderAddr := sender.Address()
	jwAddr := jetton.WalletAddress(senderAddr, minter, jwCode) // sender's own prepaid jetton wallet

	var forwardPayload *cell.Cell
	if mutate {
		// Non-nil => TransferMessage stores it as `1 | ^ref` (Either ref form); the fat reads that
		// leading 1 bit as "mutate". An empty ref suffices — the fat ignores the ref content.
		forwardPayload = cell.BeginCell().EndCell()
	}

	// response = self returns the unused GRAM as op::excesses (W5->W5, where the value must stay in the
	// pool). addr_none skips it entirely: the fat path drops excesses so a fat transfer costs the same
	// four transactions a plain jetton transfer does, the fourth being the notification instead.
	var responseAddr *address.Address
	if sendExcesses {
		responseAddr = senderAddr
	}
	body := jetton.TransferMessage(
		1,
		recipientOwner,
		responseAddr,
		jetton.InitModePayload(initMode),
		forwardTON,
		forwardPayload,
	)

	internal := &tlb.InternalMessage{
		IHRDisabled: true,
		Bounce:      true,
		DstAddr:     jwAddr,
		Amount:      value,
		Body:        body,
		StateInit:   nil,
	}

	return sender.BuildSignedBodyWithSeqno(seqno, validUntil, []walletv5.Message{{Mode: w5SendMode, Internal: internal}})
}
