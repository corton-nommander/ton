// Package jetton builds the signed WalletSpam external that performs a single TEP-74 jetton transfer.
// Shared by cmd/bot (/jetton) and cmd/spam (--mode jetton) so both produce byte-identical externals.
//
// It imports only pkg/walletspam (not pkg/pool — pool imports walletspam, so importing pool here would
// cycle). The caller picks the recipient (a random pool wallet) and passes its owner address in.
package jetton

import (
	"fmt"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

// transferOp = 0xf8a7ea5 (TEP-74 op::transfer).
const transferOp = 0xf8a7ea5

// sendModePayGasSeparately = SEND_MODE_PAY_FEES_SEPARATELY (1) | SEND_MODE_IGNORE_ERRORS (2). This is
// part of the SIGNED external body, so it must stay fixed or the pool address vector / body hash shifts.
const sendModePayGasSeparately = 1 | 2

// prepaidBalance mirrors PREPAID_BALANCE in jetton-prepaid/contracts/jetton-utils.tolk: every prepaid
// jetton wallet is born with this balance, and it is part of the wallet's state_init (hence its address).
const prepaidBalance = 1000000

// TransferMessage builds the body of an internal op::transfer to a JettonWallet. Mirrors
// wrappers/JettonWallet.ts:transferMessage.
func TransferMessage(
	jettonAmount uint64,
	to *address.Address,
	responseAddress *address.Address,
	customPayload *cell.Cell,
	forwardTONAmount uint64,
	forwardPayload *cell.Cell,
) *cell.Cell {
	b := cell.BeginCell().
		MustStoreUInt(transferOp, 32).
		MustStoreUInt(0, 64). // queryId
		MustStoreCoins(jettonAmount).
		MustStoreAddr(to).
		MustStoreAddr(responseAddress)
	if customPayload != nil {
		b = b.MustStoreUInt(1, 1).MustStoreRef(customPayload)
	} else {
		b = b.MustStoreUInt(0, 1)
	}
	b = b.MustStoreCoins(forwardTONAmount)
	if forwardPayload != nil {
		b = b.MustStoreUInt(1, 1).MustStoreRef(forwardPayload)
	} else {
		b = b.MustStoreUInt(0, 1)
	}
	return b.EndCell()
}

// WalletStateInit builds the prepaid jetton-wallet state_init, mirroring calcDeployedJettonWallet in
// jetton-prepaid/contracts/jetton-utils.tolk: data = jettonBalance(PREPAID_BALANCE) | owner | minter
// (no code ref, no split depth). Used both for the address (WalletAddress) and to auto-deploy the wallet
// (attach this init to an internal message, e.g. the pool kickoff — see pkg/deploy).
func WalletStateInit(owner, master *address.Address, walletCode *cell.Cell) *tlb.StateInit {
	dataCell := cell.BeginCell().
		MustStoreCoins(prepaidBalance).
		MustStoreAddr(owner).
		MustStoreAddr(master).
		EndCell()
	return &tlb.StateInit{Code: walletCode, Data: dataCell}
}

// WalletAddress derives the plain (non-sharded) prepaid jetton-wallet address as a pure state_init hash.
func WalletAddress(owner, master *address.Address, walletCode *cell.Cell) *address.Address {
	return WalletStateInit(owner, master, walletCode).CalcAddress(0)
}

// InitModePayload encodes internal_init_mode (see JettonWallet.tolk AskToTransfer) into an optional
// customPayload cell. Mode 0 (classic full state_init) => nil (the contract reads nil as 0). Mode 1
// (bare, no state_init) => a 2-bit cell. Any other value falls back to full state_init on-chain.
func InitModePayload(mode int32) *cell.Cell {
	if mode == 0 {
		return nil
	}
	return cell.BeginCell().MustStoreUInt(uint64(mode)&0x3, 2).EndCell()
}

// BuildTransferExternal signs an external on `owner` (a pool WalletSpam) carrying one op::transfer of 1
// jetton from owner's OWN pre-deployed jetton wallet to recipientOwner's jetton wallet.
//
// Why recipientOwner is another pool wallet (caller's choice): every pool wallet's jetton wallet is
// already deployed in-chain by Reproduce, so the transfer neither deploys a new wallet nor sheds the
// storage reserve into an uncontrolled address — the GRAM stays inside the pool. Because recipients are
// pre-deployed, initMode 1 (bare) is the cheapest correct mode; initMode 0 also works (state_init is
// ignored on an existing account). No StateInit is ever attached to op::transfer.
//
// value is a FLOAT, not the cost: responseAddress = owner, so unused GRAM returns as op::excesses.
// comment is a small random number woven into the forward text payload; validUntil bounds the external.
func BuildTransferExternal(
	owner *walletspam.WalletSpam,
	seqno uint32,
	jwCode *cell.Cell,
	minter *address.Address,
	value tlb.Coins,
	initMode int32,
	recipientOwner *address.Address,
	comment int,
	validUntil uint32,
) (*cell.Cell, error) {
	ownerAddr := owner.Address()
	internalCell, err := BuildTransferInternal(
		ownerAddr,
		jwCode,
		minter,
		value,
		initMode,
		recipientOwner,
		ownerAddr,
		comment,
	)
	if err != nil {
		return nil, err
	}

	return owner.BuildExternalBodyWithMessage(seqno, validUntil, sendModePayGasSeparately, internalCell), nil
}

// BuildTransferInternal builds the MessageRelaxed sent by a WalletSpam owner to its own prepaid
// jetton wallet. It is shared by direct externals and target-signed gasless action lists.
func BuildTransferInternal(
	ownerAddr *address.Address,
	jwCode *cell.Cell,
	minter *address.Address,
	value tlb.Coins,
	initMode int32,
	recipientOwner *address.Address,
	responseAddress *address.Address,
	comment int,
) (*cell.Cell, error) {
	jWalletAddr := WalletAddress(ownerAddr, minter, jwCode)

	commentBytes := []byte(fmt.Sprintf("%d", comment))
	forwardPayload := cell.BeginCell().
		MustStoreUInt(0, 32).
		MustStoreSlice(commentBytes, uint(len(commentBytes)*8)).
		EndCell()

	transferBody := TransferMessage(
		1,
		recipientOwner,
		responseAddress,
		InitModePayload(initMode),
		0,
		forwardPayload,
	)

	internal := &tlb.InternalMessage{
		IHRDisabled: true,
		Bounce:      true,
		DstAddr:     jWalletAddr,
		Amount:      value,
		Body:        transferBody,
		StateInit:   nil,
	}
	return tlb.ToCell(internal)
}

// BuildFatTransferExternal signs an external on `owner` (a pool WalletSpam) carrying one op::transfer of 1
// jetton from owner's OWN pre-deployed jetton wallet to `fatOwner`'s jetton wallet, with forwardTON>0 so a
// transfer_notification lands ON fatOwner — forcing the collator to walk its large storage dict (the
// long_collation split axis). Mirrors fats.BuildW5JettonTransfer but sends from a WalletSpam pool wallet
// (cmd/spam --mode jetton-fats) instead of a generated wallet-v5.
//
// mutate sets the fat's mutate flag in the forward payload (a non-empty ref-form payload whose leading
// Either bit is 1). initMode is the recipient internal_init_mode: 0 (full state_init) auto-deploys the
// fat's jetton wallet under `minter` (it is NOT pre-deployed), which is the default and only correct mode
// for a stranger recipient.
//
// response_destination is deliberately addr_none, so NO op::excesses comes back (JettonWallet.tolk only
// sends it when sendExcessesTo != null). That keeps a fat transfer at the same FOUR transactions a plain
// /jetton takes — there the fourth is the excesses, here it is the notification that lands on the fat.
// The leftover GRAM stays on the fat's jetton wallet instead of returning; it was leaving the pool either
// way, since the whole point is to touch a stranger's big-state account.
//
// This is a SIBLING of BuildTransferExternal (kept separate so the golden-locked pool→pool builder stays
// byte-stable), not a replacement.
func BuildFatTransferExternal(
	owner *walletspam.WalletSpam,
	seqno uint32,
	jwCode *cell.Cell,
	minter *address.Address,
	value tlb.Coins,
	initMode int32,
	fatOwner *address.Address,
	forwardTON uint64,
	mutate bool,
	validUntil uint32,
) (*cell.Cell, error) {
	ownerAddr := owner.Address()
	jWalletAddr := WalletAddress(ownerAddr, minter, jwCode)

	var forwardPayload *cell.Cell
	if mutate {
		// Non-nil => TransferMessage stores it as `1 | ^ref` (Either ref form); the fat reads that leading
		// 1 bit as "mutate". An empty ref suffices — the fat ignores the ref content.
		forwardPayload = cell.BeginCell().EndCell()
	}

	transferBody := TransferMessage(
		1,
		fatOwner,
		nil, // addr_none => no op::excesses back to the sender
		InitModePayload(initMode),
		forwardTON,
		forwardPayload,
	)

	internal := &tlb.InternalMessage{
		IHRDisabled: true,
		Bounce:      true,
		DstAddr:     jWalletAddr,
		Amount:      value,
		Body:        transferBody,
		StateInit:   nil,
	}
	internalCell, err := tlb.ToCell(internal)
	if err != nil {
		return nil, err
	}

	return owner.BuildExternalBodyWithMessage(seqno, validUntil, sendModePayGasSeparately, internalCell), nil
}
