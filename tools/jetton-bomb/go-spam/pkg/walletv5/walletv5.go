// Package walletv5 implements TON V5R1 (default client context) signed-external building
// against a backend.Backend. Address derivation matches @ton/ton's WalletContractV5R1.create.
package walletv5

import (
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"errors"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
)

// V5R1FinalCodeHex is the wallet-contract-v5 compiled BoC (matches @ton/ton WalletContractV5R1).
const V5R1FinalCodeHex = "b5ee9c7241021401000281000114ff00f4a413f4bcf2c80b01020120020d020148030402dcd020d749c120915b8f6320d70b1f2082106578746ebd21821073696e74bdb0925f03e082106578746eba8eb48020d72101d074d721fa4030fa44f828fa443058bd915be0ed44d0810141d721f4058307f40e6fa1319130e18040d721707fdb3ce03120d749810280b99130e070e2100f020120050c020120060902016e07080019adce76a2684020eb90eb85ffc00019af1df6a2684010eb90eb858fc00201480a0b0017b325fb51341c75c875c2c7e00011b262fb513435c280200019be5f0f6a2684080a0eb90fa02c0102f20e011e20d70b1f82107369676ebaf2e08a7f0f01e68ef0eda2edfb218308d722028308d723208020d721d31fd31fd31fed44d0d200d31f20d31fd3ffd70a000af90140ccf9109a28945f0adb31e1f2c087df02b35007b0f2d0845125baf2e0855036baf2e086f823bbf2d0882292f800de01a47fc8ca00cb1f01cf16c9ed542092f80fde70db3cd81003f6eda2edfb02f404216e926c218e4c0221d73930709421c700b38e2d01d72820761e436c20d749c008f2e09320d74ac002f2e09320d71d06c712c2005230b0f2d089d74cd7393001a4e86c128407bbf2e093d74ac000f2e093ed55e2d20001c000915be0ebd72c08142091709601d72c081c12e25210b1e30f20d74a111213009601fa4001fa44f828fa443058baf2e091ed44d0810141d718f405049d7fc8ca0040048307f453f2e08b8e14038307f45bf2e08c22d70a00216e01b3b0f2d090e2c85003cf1612f400c9ed54007230d72c08248e2d21f2e092d200ed44d0d2005113baf2d08f54503091319c01810140d721d70a00f2e08ee2c8ca0058cf16c9ed5493f2c08de20010935bdb31e1d74cd0b4d6c35e"

const (
	DefaultNetworkGlobalID int32 = -239
	WalletVersion                = 0
)

// Wallet is a V5R1 control wallet bound to a backend (LS or HTTP).
type Wallet struct {
	pub             ed25519.PublicKey
	priv            ed25519.PrivateKey
	workchain       int8
	subwallet       uint16
	networkGlobalID int32
	walletIDVal     int32 // the resolved wallet_id stored in data and signed (raw for bench wallets)
	addr            *address.Address
	code            *cell.Cell
}

// New constructs a V5R1 wallet for the given key with default client context.
// networkGlobalID = -239 unless overridden. wallet_id is the standard packed_context ^ networkGlobalID.
func New(pub ed25519.PublicKey, priv ed25519.PrivateKey, networkGlobalID int32) *Wallet {
	w := &Wallet{
		pub:             pub,
		priv:            priv,
		workchain:       0,
		subwallet:       0,
		networkGlobalID: networkGlobalID,
		code:            mustParseBoc(V5R1FinalCodeHex),
	}
	w.walletIDVal = w.walletID()
	w.finishInit()
	return w
}

// NewWithWalletID constructs a V5R1 wallet with an EXPLICIT raw wallet_id (not derived from the network
// global id). Used for the deterministically-generated bench wallets, whose state-gen counterpart stores
// a raw wallet_id (default 0); the V5R1 contract only checks stored == signed, so any value is accepted.
func NewWithWalletID(pub ed25519.PublicKey, priv ed25519.PrivateKey, walletID int32) *Wallet {
	w := &Wallet{
		pub:         pub,
		priv:        priv,
		workchain:   0,
		subwallet:   0,
		walletIDVal: walletID,
		code:        mustParseBoc(V5R1FinalCodeHex),
	}
	w.finishInit()
	return w
}

func (w *Wallet) finishInit() {
	si := tlb.StateInit{Code: w.code, Data: w.dataCell()}
	w.addr = si.CalcAddress(int(w.workchain))
}

func mustParseBoc(hexStr string) *cell.Cell {
	b, err := hex.DecodeString(hexStr)
	if err != nil {
		panic(err)
	}
	c, err := cell.FromBOC(b)
	if err != nil {
		panic(err)
	}
	return c
}

func (w *Wallet) Address() *address.Address { return w.addr }

// StateInit returns the wallet's initial state (code+data) so a first external can deploy it in place —
// used when funding a fresh control wallet and immediately having it send (SendExternalMessageWithInit).
func (w *Wallet) StateInit() *tlb.StateInit {
	return &tlb.StateInit{Code: w.code, Data: w.dataCell()}
}

// PublicKey returns the wallet's ed25519 public key.
func (w *Wallet) PublicKey() ed25519.PublicKey { return w.pub }

// walletID computes the V5R1 walletId for client context with our params.
// Mirrors @ton/ton storeWalletIdV5R1: int32(networkGlobalId) ^ int32(packed_context).
func (w *Wallet) walletID() int32 {
	c := cell.BeginCell().
		MustStoreUInt(1, 1).
		MustStoreInt(int64(w.workchain), 8).
		MustStoreUInt(uint64(WalletVersion), 8).
		MustStoreUInt(uint64(w.subwallet), 15).
		EndCell()
	ctxInt, err := c.BeginParse().LoadInt(32)
	if err != nil {
		panic(err)
	}
	return int32(ctxInt) ^ w.networkGlobalID
}

func (w *Wallet) dataCell() *cell.Cell {
	return cell.BeginCell().
		MustStoreUInt(1, 1).                    // is signature auth allowed
		MustStoreUInt(0, 32).                   // seqno
		MustStoreInt(int64(w.walletIDVal), 32). // wallet_id (signed int32)
		MustStoreSlice(w.pub, 256).             // pubkey
		MustStoreUInt(0, 1).                    // empty plugins dict
		EndCell()
}

// Message describes a single internal message to send.
type Message struct {
	Mode     uint8
	Internal *tlb.InternalMessage
}

const (
	signedExternalOp = 0x7369676e
	actionSendMsgOp  = 0x0ec3c86d
)

// SendMessages signs an external with the given messages and submits via the backend.
// validUntil defaults to now+60s if zero.
func (w *Wallet) SendMessages(ctx context.Context, b backend.Backend, validUntil uint32, messages []Message) error {
	if validUntil == 0 {
		validUntil = uint32(time.Now().Add(60 * time.Second).Unix())
	}
	seqno, err := b.GetSeqno(ctx, w.addr)
	if err != nil {
		// On uninitialized contract or transient errors we still need a seqno;
		// caller is expected to fund the wallet before first send.
		return err
	}
	body, err := w.buildSignedBody(seqno, validUntil, messages)
	if err != nil {
		return err
	}
	return b.SendExternalMessage(ctx, w.addr, body)
}

// BuildSignedBodyWithSeqno builds the signed external body for an explicit (cached) seqno, so a driver
// loop can supply the seqno from its own cache instead of fetching per send (mirrors walletspam's
// BuildExternalBodyWithMessage). SendMessages fetches the seqno itself; this is the pooled path.
func (w *Wallet) BuildSignedBodyWithSeqno(seqno uint32, validUntil uint32, messages []Message) (*cell.Cell, error) {
	return w.buildSignedBody(seqno, validUntil, messages)
}

func (w *Wallet) buildSignedBody(seqno uint32, validUntil uint32, messages []Message) (*cell.Cell, error) {
	actions, err := packV5OutActions(messages)
	if err != nil {
		return nil, err
	}
	payload := cell.BeginCell().
		MustStoreUInt(signedExternalOp, 32).
		MustStoreInt(int64(w.walletIDVal), 32).
		MustStoreUInt(uint64(validUntil), 32).
		MustStoreUInt(uint64(seqno), 32).
		MustStoreBuilder(actions)

	sig := ed25519.Sign(w.priv, payload.EndCell().Hash())

	return cell.BeginCell().
		MustStoreBuilder(payload).
		MustStoreSlice(sig, 512).
		EndCell(), nil
}

func packV5OutActions(messages []Message) (*cell.Builder, error) {
	if len(messages) > 255 {
		return nil, errors.New("v5r1: max 255 messages per external")
	}
	list := cell.BeginCell().EndCell()
	for _, m := range messages {
		if m.Internal == nil {
			return nil, errors.New("v5r1: nil internal message")
		}
		outMsg, err := tlb.ToCell(m.Internal)
		if err != nil {
			return nil, err
		}
		msg := cell.BeginCell().
			MustStoreUInt(actionSendMsgOp, 32).
			MustStoreUInt(uint64(m.Mode), 8).
			MustStoreRef(outMsg)
		list = cell.BeginCell().MustStoreRef(list).MustStoreBuilder(msg).EndCell()
	}
	return cell.BeginCell().MustStoreUInt(1, 1).MustStoreRef(list).MustStoreUInt(0, 1), nil
}
