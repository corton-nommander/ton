// Package gram builds ordinary native GRAM transfers sent by WalletSpam.
package gram

import (
	"fmt"
	"math/big"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"

	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

// RelayFeeHeadroomNano covers the target WalletSpam's compute, forwarding, and storage fees.
// Verified on testnet with a 0.001 GRAM transfer, including accrued rent and a 123-byte comment.
const RelayFeeHeadroomNano = 1_000_000

// DefaultRelayValue funds a native transfer without the jetton relay's much larger float.
// Derive it from the transfer value so changing GRAM_TRANSFER_VALUE keeps the same fee headroom.
func DefaultRelayValue(transferValue tlb.Coins) tlb.Coins {
	return tlb.FromNanoTON(new(big.Int).Add(transferValue.Nano(), big.NewInt(RelayFeeHeadroomNano)))
}

// BuildTransferInternal builds an ordinary non-bounceable transfer with a random text comment.
func BuildTransferInternal(destination *address.Address, value tlb.Coins) (*cell.Cell, error) {
	return BuildTransferInternalWithComment(destination, value, RandomComment())
}

// BuildTransferInternalWithComment builds a transfer carrying the standard TON text-comment body:
// uint32(0) followed by inline UTF-8 bytes. The body always fits in one cell and has no refs.
func BuildTransferInternalWithComment(destination *address.Address, value tlb.Coins, comment string) (*cell.Cell, error) {
	body, err := buildCommentBody(comment)
	if err != nil {
		return nil, err
	}
	return tlb.ToCell(&tlb.InternalMessage{
		IHRDisabled: true,
		Bounce:      false,
		DstAddr:     destination,
		Amount:      value,
		Body:        body,
	})
}

func buildCommentBody(comment string) (*cell.Cell, error) {
	if len(comment) > MaxCommentSize {
		return nil, fmt.Errorf("gram comment is %d bytes, max %d", len(comment), MaxCommentSize)
	}
	return cell.BeginCell().
		MustStoreUInt(CommentOpcode, 32).
		MustStoreSlice([]byte(comment), uint(len(comment)*8)).
		EndCell(), nil
}

// BuildTransferExternal wraps one ordinary GRAM transfer in the sender's signed external.
func BuildTransferExternal(
	sender *walletspam.WalletSpam,
	seqno uint32,
	validUntil uint32,
	destination *address.Address,
	value tlb.Coins,
) (*cell.Cell, error) {
	internal, err := BuildTransferInternal(destination, value)
	if err != nil {
		return nil, err
	}
	return sender.BuildExternalBodyWithMessage(
		seqno,
		validUntil,
		walletspam.DefaultSendMode,
		internal,
	), nil
}
