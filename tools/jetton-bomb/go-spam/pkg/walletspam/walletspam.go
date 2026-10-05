// Package walletspam mirrors the Tolk WalletSpam contract (jetton-prepaid/contracts/WalletSpam.tolk).
// Data layout (WalletSpamStorage.toCell, declaration order — cell fields ride as refs):
//
//	seqno(32) | subwallet_id(32) | pubkey(256) | minter(MsgAddress) | ^jettonWalletCode
//
// External body = signature(512) || subwallet_id(32) | valid_until(32) | seqno(32) | (sendMode(8) | ^msg)*
package walletspam

import (
	"crypto/ed25519"
	"errors"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

const (
	SignedInternalOp     = 0x73696e74
	ActionSendMsgOp      = 0x0ec3c86d
	SendModeIgnoreErrors = uint8(2)
	DefaultSendMode      = uint8(1) | SendModeIgnoreErrors // PAY_FEES_SEPARATELY | IGNORE_ERRORS
)

// Action is one action_send_msg node in a Wallet V5-compatible c5 OutList.
type Action struct {
	Mode    uint8
	Message *cell.Cell
}

type WalletSpam struct {
	SubwalletID      uint32
	PubKey           ed25519.PublicKey
	PrivKey          ed25519.PrivateKey
	Minter           *address.Address
	JettonWalletCode *cell.Cell
	Code             *cell.Cell
	addr             *address.Address
}

func New(subwalletID uint32, pub ed25519.PublicKey, priv ed25519.PrivateKey, minter *address.Address, jwCode, code *cell.Cell) *WalletSpam {
	w := &WalletSpam{
		SubwalletID:      subwalletID,
		PubKey:           pub,
		PrivKey:          priv,
		Minter:           minter,
		JettonWalletCode: jwCode,
		Code:             code,
	}
	data := w.dataCell()
	si := tlb.StateInit{Code: code, Data: data}
	w.addr = si.CalcAddress(0)
	return w
}

func (w *WalletSpam) Address() *address.Address {
	return w.addr
}

func (w *WalletSpam) StateInit() *tlb.StateInit {
	return &tlb.StateInit{Code: w.Code, Data: w.dataCell()}
}

func (w *WalletSpam) dataCell() *cell.Cell {
	return cell.BeginCell().
		MustStoreUInt(0, 32). // seqno
		MustStoreUInt(uint64(w.SubwalletID), 32).
		MustStoreSlice(w.PubKey, 256).
		MustStoreAddr(w.Minter).
		MustStoreRef(w.JettonWalletCode).
		EndCell()
}

// BuildExternalBody returns the cell that goes into ExternalMessage.Body.
// Empty actions: just (subwallet_id, valid_until, seqno) + signature prefix.
func (w *WalletSpam) BuildExternalBody(seqno uint32, validUntil uint32) *cell.Cell {
	msg := cell.BeginCell().
		MustStoreUInt(uint64(w.SubwalletID), 32).
		MustStoreUInt(uint64(validUntil), 32).
		MustStoreUInt(uint64(seqno), 32).
		EndCell()

	sig := ed25519.Sign(w.PrivKey, msg.Hash())

	return cell.BeginCell().
		MustStoreSlice(sig, 512).
		MustStoreUInt(uint64(w.SubwalletID), 32).
		MustStoreUInt(uint64(validUntil), 32).
		MustStoreUInt(uint64(seqno), 32).
		EndCell()
}

// BuildExternalBodyWithMessage signs and returns an external body that wraps a single internal
// MessageRelaxed with the given send mode, mirroring WalletSpam.createTransfer in wrappers/WalletSpam.ts.
// internalMsg is the relaxed message cell (storeMessageRelaxed result), not the inner body.
func (w *WalletSpam) BuildExternalBodyWithMessage(seqno uint32, validUntil uint32, sendMode uint8, internalMsg *cell.Cell) *cell.Cell {
	msg := cell.BeginCell().
		MustStoreUInt(uint64(w.SubwalletID), 32).
		MustStoreUInt(uint64(validUntil), 32).
		MustStoreUInt(uint64(seqno), 32).
		MustStoreUInt(uint64(sendMode), 8).
		MustStoreRef(internalMsg).
		EndCell()

	sig := ed25519.Sign(w.PrivKey, msg.Hash())

	return cell.BeginCell().
		MustStoreSlice(sig, 512).
		MustStoreUInt(uint64(w.SubwalletID), 32).
		MustStoreUInt(uint64(validUntil), 32).
		MustStoreUInt(uint64(seqno), 32).
		MustStoreUInt(uint64(sendMode), 8).
		MustStoreRef(internalMsg).
		EndCell()
}

// BuildInternalSignedRequest builds the W5-compatible `sint` body accepted by WalletSpam's internal
// entrypoint. The signature covers the opcode, wallet id, validity, shared seqno, and the complete c5
// action tree. WalletSpam supports send-message actions only and no W5 extension actions.
func (w *WalletSpam) BuildInternalSignedRequest(seqno uint32, validUntil uint32, actions []Action) (*cell.Cell, error) {
	if len(actions) > 255 {
		return nil, errors.New("walletspam: max 255 signed-internal actions")
	}

	list := cell.BeginCell().EndCell()
	for _, action := range actions {
		if action.Message == nil {
			return nil, errors.New("walletspam: nil signed-internal action message")
		}
		if action.Mode&SendModeIgnoreErrors == 0 {
			return nil, errors.New("walletspam: signed-internal actions must use IGNORE_ERRORS send mode")
		}
		list = cell.BeginCell().
			MustStoreRef(list).
			MustStoreUInt(ActionSendMsgOp, 32).
			MustStoreUInt(uint64(action.Mode), 8).
			MustStoreRef(action.Message).
			EndCell()
	}

	payload := cell.BeginCell().
		MustStoreUInt(SignedInternalOp, 32).
		MustStoreUInt(uint64(w.SubwalletID), 32).
		MustStoreUInt(uint64(validUntil), 32).
		MustStoreUInt(uint64(seqno), 32)
	if len(actions) == 0 {
		payload = payload.MustStoreUInt(0, 1)
	} else {
		payload = payload.MustStoreUInt(1, 1).MustStoreRef(list)
	}
	payload = payload.MustStoreUInt(0, 1) // no W5 extra actions
	sig := ed25519.Sign(w.PrivKey, payload.EndCell().Hash())

	return cell.BeginCell().
		MustStoreBuilder(payload).
		MustStoreSlice(sig, 512).
		EndCell(), nil
}

// BuildRelayedExternal builds a relayer-signed external that sends a funded internal carrying a
// target-signed request. Backend acceptance confirms the outer submission, not downstream execution.
func BuildRelayedExternal(
	relayer *WalletSpam,
	relayerSeqno uint32,
	target *WalletSpam,
	targetSeqno uint32,
	validUntil uint32,
	relayValue tlb.Coins,
	actions []Action,
) (*cell.Cell, error) {
	request, err := target.BuildInternalSignedRequest(targetSeqno, validUntil, actions)
	if err != nil {
		return nil, err
	}
	relayMessage, err := tlb.ToCell(&tlb.InternalMessage{
		IHRDisabled: true,
		Bounce:      true,
		DstAddr:     target.Address(),
		Amount:      relayValue,
		Body:        request,
	})
	if err != nil {
		return nil, err
	}
	return relayer.BuildExternalBodyWithMessage(
		relayerSeqno,
		validUntil,
		DefaultSendMode,
		relayMessage,
	), nil
}
