package highload

import (
	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

// BuildRelayForTarget wraps a WalletSpam target's signed `sint` action in a funded internal and has the
// SHARD-MATCHED relayer sign the outer external. It returns the relayer address to post to and the signed
// external body.
//
// Retained for direct-relay parity tests and tooling. Production bot/CLI gasless modes exclusively
// use Batcher, which wraps BuildRelayMessage results in an always-self-message batch external.
//
// Routing: set[Shard(target)] keeps the relay hop intra-shard (the relayer and the target share the
// basechain top two bits). The highload pays the gas of both hops (SendModeRelay =
// pay-fees-separately|ignore-errors), so the target receives the full relayValue and needs no GRAM of
// its own. The target's seqno is consumed by the sint exactly as its own external would; the highload
// itself has no seqno (query_id replay protection), so one relayer serves many targets concurrently.
func BuildRelayForTarget(
	set [NumShards]*HighloadWallet,
	target *walletspam.WalletSpam,
	targetSeqno uint32,
	validUntil uint32,
	relayValue tlb.Coins,
	actionMessage *cell.Cell,
	createdAt uint32,
) (*address.Address, *cell.Cell, error) {
	relayMsg, err := BuildRelayMessage(target, targetSeqno, validUntil, relayValue, actionMessage)
	if err != nil {
		return nil, nil, err
	}
	hl := set[Shard(target.Address())]
	ext, err := hl.BuildRelayExternal(relayMsg, SendModeRelay, createdAt)
	if err != nil {
		return nil, nil, err
	}
	return hl.Address(), ext, nil
}

// BuildRelayMessage prepares one signed, funded sint for inclusion in a highload batch.
func BuildRelayMessage(target *walletspam.WalletSpam, targetSeqno, validUntil uint32, relayValue tlb.Coins, actionMessage *cell.Cell) (*cell.Cell, error) {
	request, err := target.BuildInternalSignedRequest(targetSeqno, validUntil,
		[]walletspam.Action{{Mode: walletspam.DefaultSendMode, Message: actionMessage}})
	if err != nil {
		return nil, err
	}
	relayMsg, err := tlb.ToCell(&tlb.InternalMessage{
		IHRDisabled: true,
		Bounce:      true,
		DstAddr:     target.Address(),
		Amount:      relayValue,
		Body:        request,
	})
	return relayMsg, err
}
