// Package deploy stands up the whole WalletSpam pool from Go alone — no acton, no minter contract, no
// deployer wallet. It mirrors jetton-prepaid/scripts/utils/walletspam.tolk `buildKickoffExternal`
// byte-for-byte (asserted by pkg/deploy/parity_test.go against a Tolk-generated vector).
//
// Two on-chain steps, driven by the caller (e.g. cmd/deploy + the devnet `ton` script):
//  1. Fund wallet0 with Wallet0Funding() GRAM (a plain transfer from any funded source — on the devnet,
//     the genesis main-wallet via wallet.fif). wallet0 stays UNINITIALIZED, just holding balance.
//  2. Send ONE pool-key-signed external to wallet0 carrying wallet0's OWN state_init (KickoffBody +
//     Wallet0().StateInit()). That external deploys wallet0 AND runs its onExternalMessage, which fires
//     wallet0's own prepaid-jetton deploy + the Reproduce that replicates the rest of the pool in-chain.
//
// The minter is NOT deployed: prepaid jetton transfers never call the minter, so its address is only a
// storage parameter fixing the pool/jetton addresses. Pick any minter address; use the SAME one for the
// spammer (WALLET_MINTER).
package deploy

import (
	"crypto/ed25519"
	"fmt"
	"math/big"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/jetton"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

// Op codes + send modes, mirroring the Tolk contracts / kickoff builder.
const (
	opReproduce  = 0xba5e5d50 // messages.tolk Reproduce
	opTopUpGrams = 0xd372158c // messages.tolk TopUpGrams (empty body)

	sendModePayFeesSeparately = 1 // SEND_MODE_PAY_FEES_SEPARATELY — wallet0's own jetton deploy
	sendModeRegular           = 0 // SEND_MODE_REGULAR — the Reproduce hop to wallet1

	// ValidUntilMax matches VALID_UNTIL in walletspam.tolk: WalletSpam rejects validUntil <= now, and
	// seqno (single-use) prevents replay, so a fixed far-future value is safe in broadcast + parity.
	ValidUntilMax = 0xFFFFFFFF
)

// Params are the single funding/replication levers, identical to the acton deploy env
// (WALLET_VALUE / JETTON_VALUE / FEE_SLACK / DEPLOY_CHUNK / DEPLOY_JETTON).
type Params struct {
	Count          int    // pool size; deploys wallets [StartSubwallet, Count]
	StartSubwallet uint32 // driver subwallet the kickoff fires from (0 = wallet0, the canonical full deploy).
	// A >0 value starts a SECOND parallel reproduce chain covering (StartSubwallet, Count] from a separately
	// funded mid-range driver — overlaps the wallet0 chain's range (both re-deploy it), so use knowingly.
	Chunk        uint16    // children per driver hop (1..253)
	DeployJetton bool      // deploy each wallet's prepaid jetton wallet in-chain
	WalletValue  tlb.Coins // GRAM each pool wallet keeps
	JettonValue  tlb.Coins // GRAM forwarded to deploy each jetton wallet
	FeeSlack     tlb.Coins // per-wallet fee headroom (rolls to the tail wallet, recoverable)
	ValidUntil   uint32    // external validity bound (default ValidUntilMax)
}

// Deployer builds the kickoff for a pool keyed by (pool keypair, minter, JettonWallet code, WalletSpam code).
type Deployer struct {
	pub     ed25519.PublicKey
	priv    ed25519.PrivateKey
	minter  *address.Address
	jwCode  *cell.Cell
	code    *cell.Cell
	p       Params
	wallet0 *walletspam.WalletSpam
}

// New builds a Deployer. code = WalletSpam code, jwCode = JettonWallet code (both from pkg/walletspam).
func New(pub ed25519.PublicKey, priv ed25519.PrivateKey, minter *address.Address, jwCode, code *cell.Cell, p Params) *Deployer {
	if p.ValidUntil == 0 {
		p.ValidUntil = ValidUntilMax
	}
	return &Deployer{
		pub:     pub,
		priv:    priv,
		minter:  minter,
		jwCode:  jwCode,
		code:    code,
		p:       p,
		wallet0: walletspam.New(p.StartSubwallet, pub, priv, minter, jwCode, code),
	}
}

// Wallet0 is the pool's driver wallet (subwallet 0) — the one to fund and to send the kickoff to.
func (d *Deployer) Wallet0() *walletspam.WalletSpam { return d.wallet0 }

// PoolWalletAddress returns the WalletSpam address for subwallet id (a pure state_init hash over
// seqno|subwallet|pubkey|minter|^jwCode).
func (d *Deployer) PoolWalletAddress(id uint32) *address.Address {
	return walletspam.New(id, d.pub, d.priv, d.minter, d.jwCode, d.code).Address()
}

// JettonWalletAddress returns the prepaid jetton-wallet address owned by pool wallet id. Under
// --init-mode 1 the spammer transfers to these bare (no state_init), so they must already exist.
func (d *Deployer) JettonWalletAddress(id uint32) *address.Address {
	return jetton.WalletAddress(d.PoolWalletAddress(id), d.minter, d.jwCode)
}

// ReadinessSamples returns up to n sample addresses spread evenly across subwallets [0, Count] — the LAST
// is always the terminal wallet Count — along with their ids and the get-method to probe them. broadcast
// probes these each poll to render a progress bar and treats the terminal (last) sample as the completion
// signal: it is the causally-last wallet in the reproduce chain, so its deployment means the whole pool is
// up. Cheap: n is a small fixed handful, not a full [0,Count] sweep. When DeployJetton the addresses are
// prepaid jetton wallets (probe get_wallet_data — the recipients a bare/--init-mode-1 transfer needs);
// otherwise the WalletSpam wallets themselves (probe seqno).
func (d *Deployer) ReadinessSamples(n int) (ids []uint32, addrs []*address.Address, getMethod string) {
	count := d.p.Count
	if n < 2 {
		n = 2
	}
	if n > count+1 {
		n = count + 1 // at most one sample per wallet
	}
	ids = make([]uint32, 0, n)
	var last int64 = -1
	for j := 0; j < n; j++ {
		id := int64(j) * int64(count) / int64(n-1) // 0 .. count, evenly spaced, non-decreasing
		if id == last {
			continue // dedup (only bites when n ~ count+1)
		}
		last = id
		ids = append(ids, uint32(id))
	}
	getMethod = "seqno"
	if d.p.DeployJetton {
		getMethod = "get_wallet_data"
	}
	addrs = make([]*address.Address, len(ids))
	for i, id := range ids {
		if d.p.DeployJetton {
			addrs[i] = d.JettonWalletAddress(id)
		} else {
			addrs[i] = d.PoolWalletAddress(id)
		}
	}
	return ids, addrs, getMethod
}

func (d *Deployer) perWallet() *big.Int {
	return new(big.Int).Add(d.p.WalletValue.Nano(), d.p.FeeSlack.Nano())
}

// descendants = Count - StartSubwallet — the number of wallets below the driver this kickoff drives.
func (d *Deployer) descendants() int64 { return int64(d.p.Count) - int64(d.p.StartSubwallet) }

// Wallet0Funding = (descendants+1) * (WalletValue+FeeSlack): the driver keeps one share and forwards
// `descendants` shares down its reproduce chain. This is the amount to fund the driver before the kickoff.
func (d *Deployer) Wallet0Funding() tlb.Coins {
	n := new(big.Int).Mul(big.NewInt(d.descendants()+1), d.perWallet())
	return tlb.FromNanoTON(n)
}

// reproFunding = descendants * perWallet — the value the driver forwards to its first child.
func (d *Deployer) reproFunding() tlb.Coins {
	n := new(big.Int).Mul(big.NewInt(d.descendants()), d.perWallet())
	return tlb.FromNanoTON(n)
}

// jettonDeployMsg = wallet0's own prepaid-jetton deploy (bounce=false, value=JettonValue, TopUpGrams body,
// jetton-wallet state_init attached so it auto-deploys). Mirrors calcDeployedJettonWallet + TopUpGrams{}.
func (d *Deployer) jettonDeployMsg() (*cell.Cell, error) {
	owner := d.wallet0.Address()
	si := jetton.WalletStateInit(owner, d.minter, d.jwCode)
	body := cell.BeginCell().MustStoreUInt(opTopUpGrams, 32).EndCell()
	msg := &tlb.InternalMessage{
		IHRDisabled: true,
		Bounce:      false, // BounceMode.NoBounce
		DstAddr:     si.CalcAddress(0),
		Amount:      d.p.JettonValue,
		StateInit:   si,
		Body:        body,
	}
	return tlb.ToCell(msg)
}

// reproduceMsg = the Reproduce hop to wallet1 (bounce=true, value=reproFunding, wallet1 state_init attached
// so it auto-deploys). Body layout mirrors messages.tolk Reproduce exactly.
func (d *Deployer) reproduceMsg() (*cell.Cell, error) {
	w1 := walletspam.New(d.p.StartSubwallet+1, d.pub, d.priv, d.minter, d.jwCode, d.code)
	body := cell.BeginCell().
		MustStoreUInt(opReproduce, 32).
		MustStoreUInt(uint64(d.p.Count), 32).          // lastSubwallet
		MustStoreUInt(uint64(d.p.StartSubwallet), 32). // fromSubwallet (this driver)
		MustStoreUInt(uint64(d.p.Chunk), 16).
		MustStoreBoolBit(d.p.DeployJetton).
		MustStoreBigCoins(d.p.WalletValue.Nano()).
		MustStoreBigCoins(d.p.JettonValue.Nano()).
		EndCell()
	msg := &tlb.InternalMessage{
		IHRDisabled: true,
		Bounce:      true, // BounceMode.Only256BitsOfBody
		DstAddr:     w1.Address(),
		Amount:      d.reproFunding(),
		StateInit:   w1.StateInit(),
		Body:        body,
	}
	return tlb.ToCell(msg)
}

// KickoffBody returns the signed sig-first external body for wallet0, identical to the acton
// buildKickoffExternal output: signature(512) ‖ subwallet(0)|validUntil|seqno(0)|(sendMode|^msg)*.
// Order: wallet0 jetton deploy (mode 1, if DeployJetton) then Reproduce (mode 0). Attach
// Wallet0().StateInit() to the external in-message when broadcasting (wallet0 is uninitialized).
func (d *Deployer) KickoffBody() (*cell.Cell, error) {
	var jettonCell *cell.Cell
	if d.p.DeployJetton {
		c, err := d.jettonDeployMsg()
		if err != nil {
			return nil, fmt.Errorf("jetton deploy msg: %w", err)
		}
		jettonCell = c
	}
	repro, err := d.reproduceMsg()
	if err != nil {
		return nil, fmt.Errorf("reproduce msg: %w", err)
	}

	// content() is the signed portion (everything after the signature). Built fresh each call because a
	// Builder is consumed by EndCell; once to hash, once to prepend the signature.
	content := func() *cell.Builder {
		b := cell.BeginCell().
			MustStoreUInt(uint64(d.p.StartSubwallet), 32). // driver subwalletId
			MustStoreUInt(uint64(d.p.ValidUntil), 32).
			MustStoreUInt(0, 32) // seqno 0
		if jettonCell != nil {
			b.MustStoreUInt(sendModePayFeesSeparately, 8).MustStoreRef(jettonCell)
		}
		b.MustStoreUInt(sendModeRegular, 8).MustStoreRef(repro)
		return b
	}

	signed := content().EndCell()
	sig := ed25519.Sign(d.priv, signed.Hash())
	return cell.BeginCell().MustStoreSlice(sig, 512).MustStoreBuilder(content()).EndCell(), nil
}
