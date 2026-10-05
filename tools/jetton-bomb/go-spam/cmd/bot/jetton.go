package main

import (
	mathrand "math/rand"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/drain"
	"github.com/victor/jetton-bomb-go-spam/pkg/jetton"
	"github.com/victor/jetton-bomb-go-spam/pkg/pool"
)

// buildJettonExternalForEntry signs an external on the WalletSpam at e that transfers 1 jetton (with
// r.jettonValue GRAM attached, JETTON_TRANSFER_VALUE env, default 0.05) from e's own pre-deployed jetton
// wallet to a RANDOM OTHER POOL WALLET. Recipient, comment and validUntil are chosen here; the actual
// cell is built by the shared pkg/jetton (same builder cmd/spam uses), so both stay byte-identical.
//
// Pool recipient (not a random stranger): every pool wallet's jetton wallet is pre-deployed by Reproduce,
// so the transfer neither deploys a new wallet nor sheds the storage reserve outside the pool — GRAM stays
// in. /initmode 1 (bare) is then the cheapest correct mode. See pkg/jetton for the full rationale.
func (r *Runtime) buildJettonExternalForEntry(jWalletCode *cell.Cell, tracker *drain.Tracker, e *pool.Entry, seqno uint32, poolStart, poolCount int, chosen ...*address.Address) (*cell.Cell, error) {
	// Random recipient owner = another pool WalletSpam. A rare self-transfer (dest == owner) is harmless.
	// When /check_balance is on, draw the recipient from the live (non-drained) set so GRAM keeps flowing
	// among funded wallets and the drained accounting stays consistent.
	// Recipient stays inside the run's sub-pool, so the GRAM keeps circulating within the wallets this run
	// drives (the tracker's live set is relative to the same window).
	var dest *address.Address
	if len(chosen) > 0 {
		dest = chosen[0]
	}
	if dest == nil {
		recipientID := mathrand.Intn(poolCount)
		if tracker != nil && r.checkBalance.Load() {
			recipientID = tracker.RandomLive()
		}
		dest = r.initSpamPool().Get(poolStart + recipientID).Wallet.Address()
	}
	validUntil := uint32(time.Now().Unix() + 120)
	return jetton.BuildTransferExternal(
		e.Wallet,
		seqno,
		jWalletCode,
		r.jettonMinter,
		r.jettonValue,
		r.jettonInitMode.Load(),
		dest,
		mathrand.Intn(1_000_000),
		validUntil,
	)
}
