// Package highload wraps the Highload Wallet V3 R1 contract (contracts/highload_v3r1.fc), used as the
// gasless RELAYER in the WalletSpam load test. Unlike a WalletSpam relayer (one seqno, serialized),
// a highload has NO seqno: replay protection is a (shift,bit_number) query_id into a per-timeout-window
// dict, so any number of relays can be signed and submitted concurrently from one wallet. We use four
// of them — one per basechain top-two-bit shard bucket — and route each WalletSpam target to the highload in
// its own shard so the relay hop stays intra-shard.
//
// The code cell is the canonical Fragment highload-v3r1 BoC pulled from mainnet (NOT recompiled), so the
// address matches the real contract everywhere it is redeployed (devnet/testnet/mainnet).
//
// Data layout (get_public_key / recv_external order):
//
//	public_key(256) | subwallet_id(32) | old_queries:HashmapE | queries:HashmapE | last_clean_time(64) | timeout(22)
//
// External body = signature(512) | ^payload, signed over payload.hash() (canonical highload-v3 layout —
// the fetched code hashes byte-identical to ton-blockchain highload-wallet-contract-v3).
// payload = subwallet_id(32) | ^message_to_send | send_mode(8) | query_id(23) | created_at(64) | timeout(22)
package highload

import (
	"crypto/ed25519"
	crand "crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"sync/atomic"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

// CodeHex is the highload-v3r1 code BoC (hex), fetched from a live mainnet Fragment highload wallet.
// internal_transfer op 0xae42e5a4 is visible inline (…8210ae42e5a4…).
const CodeHex = "b5ee9c7201021001000228000114ff00f4a413f4bcf2c80b010201200203020148040501f6f2d48308d718d121f900ed44d0d3ffd31ff404f404d33fd315d1f82321a15220b98e12336df82324aa00a112b9926d32de58f82301de541675f910f2a106d0d31fd4d307d30cd309d33fd315d15168baf2a2515abaf2a6f8232aa15250bcf2a304f823bbf2a35304800df40f6fa199d024d721d70a00f2649130e20e0078d020d74bc00101c060b0915be101d0d3030171b0915be0fa4030f828c705b39130e0d31f018210ae42e5a4ba9d8040d721d74cf82a01ed55fb04e030020120060702027308090201200c0d0011adce76a2686b85ffc00201200a0b001aabb6ed44d0810122d721d70b3f0018aa3bed44d08307d721d70b1f001bb9a6eed44d0810162d721d70b15800e5b8bf2eda2edfb21ab09028409b0ed44d0810120d721f404f404d33fd315d1058e1bf82325a15210b99f326df82305aa0015a112b992306dde923033e2923033e25230800df40f6fa19ed021d721d70a00955f037fdb31e09130e259800df40f6fa19cd001d721d70a00937fdb31e0915be270801fe5309800df40f6fa18e13d05004d718d20001f264c858cf16cf8301cf168e1030c824cf40cf8384095005a1a514cf40e2f800c94039800df41704c8cbff13cb1ff40012f40012cb3f12cb15c9ed54f80f21d0d30001f265d3020171b0925f03e0fa4001d70b01c000f2a5fa4031fa0031f401fa0031fa00318060d721d300010f0020f265d2000193d431d19130e272b1fb00"

const (
	// DefaultTimeout is the highload query-id replay window (seconds). Also the external validity horizon:
	// created_at must be within [now-timeout, now]. Larger => wider validity but a bigger replay dict; the
	// dict rotates roughly every `timeout`. 1h comfortably covers the paced batch external rate.
	DefaultTimeout uint32 = 3600

	// SendModeRelay = PAY_GAS_SEPARATELY(1) | IGNORE_ERRORS(2): the relay hop's forward fee is paid from
	// the highload balance so the target WalletSpam receives the full relay value, and an action-phase
	// hiccup can't fault the highload.
	SendModeRelay uint8 = 1 | 2

	// CreatedAtBackdateSec is how far behind wall clock we stamp created_at. The contract enforces
	//
	//	throw_unless(35, created_at >  now() - timeout)   // not too old
	//	throw_unless(35, created_at <= now())             // NOT in the future
	//
	// and `now()` in TVM is the BLOCK's gen_utime, which trails wall clock by up to one block interval
	// (~3s on testnet). Stamping created_at = time.Now() therefore lands in the future for most of each
	// block window and the external is rejected with exit code 35 — measured as only ~22-30% accepted.
	// Back-dating costs nothing: the other bound leaves timeout (3600s) of slack, so 10s covers several
	// block intervals plus modest clock skew between the sender and the validating node.
	CreatedAtBackdateSec uint32 = 10

	pubKeyBits      = 256
	subwalletIDBits = 32
	timestampBits   = 64
	timeoutBits     = 22
	sendModeBits    = 8
	queryIDBits     = 23

	// The on-chain replay dict splits query_id into shift = qid>>10 (13-bit dict key) and bit_number =
	// qid&1023 (position in a 1023-bit cell), so bit_number must stay ≤ 1022. We hand out a sequential
	// index i and pack it as (i/1023)<<10 | (i%1023): valid bit_number, shift < 2^13. maxIndex indices
	// exist before the space wraps — far beyond a run's paced batch external count.
	usableBits = 1023
	maxShift   = 1 << 13
	maxIndex   = maxShift * usableBits // 8,380,416
)

var errNilMessage = errors.New("highload: nil message_to_send")

// CreatedAtNow returns the created_at to sign into a relay: wall clock back-dated by
// CreatedAtBackdateSec. ALWAYS use this instead of time.Now() for created_at — the contract rejects any
// timestamp ahead of the validating block's gen_utime with exit code 35 (see CreatedAtBackdateSec).
func CreatedAtNow() uint32 {
	return uint32(time.Now().Unix()) - CreatedAtBackdateSec
}

// HighloadWallet is one deployed highload-v3r1 relayer with a monotonic query-id allocator.
type HighloadWallet struct {
	subwalletID uint32
	timeout     uint32
	pub         ed25519.PublicKey
	priv        ed25519.PrivateKey
	code        *cell.Cell
	addr        *address.Address
	qid         atomic.Uint64
}

// New builds a highload-v3r1 wallet for the given key and subwallet id. priv may be nil for
// address-only use (grinding); relay building needs a real priv. timeout=0 => DefaultTimeout.
func New(subwalletID uint32, pub ed25519.PublicKey, priv ed25519.PrivateKey, timeout uint32) *HighloadWallet {
	if timeout == 0 {
		timeout = DefaultTimeout
	}
	w := &HighloadWallet{
		subwalletID: subwalletID,
		timeout:     timeout,
		pub:         pub,
		priv:        priv,
		code:        mustCode(),
	}
	w.qid.Store(randomQueryIDStart())
	si := tlb.StateInit{Code: w.code, Data: w.dataCell()}
	w.addr = si.CalcAddress(0)
	return w
}

// randomQueryIDStart picks a uniformly random starting index in the id space instead of starting at 0.
//
// The on-chain replay dict remembers every query_id for between `timeout` and 2*timeout (queries ages
// into old_queries, which is dropped only after another timeout). A counter that restarts at 0 therefore
// REPLAYS the ids the previous process already spent, and every one of those relays is rejected with exit
// code 36 (already_executed) until the counter climbs past the old high-water mark — seen live on testnet
// after a bot restart.
//
// Random (not clock-derived) on purpose: a clock-derived seed hands two processes started in the same
// second the SAME start, so the bot and a concurrent cmd/spam run would collide head-on. With ~8.38M ids
// and at most ~22k spent per relayer per window (≈6 relays/s), a random start overlaps a used band with
// probability ~0.3%, and then only partially.
func randomQueryIDStart() uint64 {
	var b [8]byte
	if _, err := crand.Read(b[:]); err != nil {
		// Fall back to the clock: worse against concurrent starts, still far better than a fixed 0.
		return uint64(time.Now().UnixNano()) % maxIndex
	}
	return binary.BigEndian.Uint64(b[:]) % maxIndex
}

// SetQueryIDStart pins the allocator's next index, overriding the random start. Only for tests and
// reproducible vectors (the parity test needs a known first query_id) — production code must keep the
// random start, or restarts replay ids (see randomQueryIDStart).
func (w *HighloadWallet) SetQueryIDStart(index uint64) {
	w.qid.Store(index % maxIndex)
}

var codeCell *cell.Cell

func mustCode() *cell.Cell {
	if codeCell != nil {
		return codeCell
	}
	raw, err := hex.DecodeString(CodeHex)
	if err != nil {
		panic(err)
	}
	c, err := cell.FromBOC(raw)
	if err != nil {
		panic(err)
	}
	codeCell = c
	return codeCell
}

func (w *HighloadWallet) Address() *address.Address { return w.addr }
func (w *HighloadWallet) SubwalletID() uint32       { return w.subwalletID }
func (w *HighloadWallet) Timeout() uint32           { return w.timeout }
func (w *HighloadWallet) PubKey() ed25519.PublicKey { return w.pub }
func (w *HighloadWallet) Code() *cell.Cell          { return w.code }

func (w *HighloadWallet) StateInit() *tlb.StateInit {
	return &tlb.StateInit{Code: w.code, Data: w.dataCell()}
}

func (w *HighloadWallet) dataCell() *cell.Cell {
	return cell.BeginCell().
		MustStoreSlice(w.pub, pubKeyBits).
		MustStoreUInt(uint64(w.subwalletID), subwalletIDBits).
		MustStoreUInt(0, 1).             // old_queries: empty HashmapE
		MustStoreUInt(0, 1).             // queries: empty HashmapE
		MustStoreUInt(0, timestampBits). // last_clean_time
		MustStoreUInt(uint64(w.timeout), timeoutBits).
		EndCell()
}

// nextQueryID hands out a unique 23-bit query_id per call, monotonic and concurrency-safe. It packs a
// sequential index as (i/1023)<<10 | (i%1023) so bit_number (low 10 bits) never reaches the reserved 1023,
// and wraps after maxIndex (~8.38M) — far beyond any single run's relay count.
func (w *HighloadWallet) nextQueryID() uint32 {
	i := (w.qid.Add(1) - 1) % maxIndex
	shift := i / usableBits
	bitNumber := i % usableBits
	return uint32(shift<<10 | bitNumber)
}

// BuildRelayExternal signs an external that makes this highload send messageToSend (a fully-serialized
// relaxed InternalMessage cell — e.g. a funded internal to a WalletSpam carrying its `sint` request) with
// sendMode. createdAt must be the current unix time (validated on-chain against the timeout window).
func (w *HighloadWallet) BuildRelayExternal(messageToSend *cell.Cell, sendMode uint8, createdAt uint32) (*cell.Cell, error) {
	return w.buildRelayExternal(messageToSend, sendMode, createdAt, w.nextQueryID())
}

func (w *HighloadWallet) buildRelayExternal(messageToSend *cell.Cell, sendMode uint8, createdAt, queryID uint32) (*cell.Cell, error) {
	if messageToSend == nil {
		return nil, errNilMessage
	}
	if w.priv == nil {
		return nil, errors.New("highload: wallet has no private key")
	}
	payload := cell.BeginCell().
		MustStoreUInt(uint64(w.subwalletID), subwalletIDBits).
		MustStoreRef(messageToSend).
		MustStoreUInt(uint64(sendMode), sendModeBits).
		MustStoreUInt(uint64(queryID), queryIDBits).
		MustStoreUInt(uint64(createdAt), timestampBits).
		MustStoreUInt(uint64(w.timeout), timeoutBits).
		EndCell()

	sig := ed25519.Sign(w.priv, payload.Hash())

	return cell.BeginCell().
		MustStoreSlice(sig, 512).
		MustStoreRef(payload).
		EndCell(), nil
}
