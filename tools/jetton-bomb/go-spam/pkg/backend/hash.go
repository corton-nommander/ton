package backend

import (
	"encoding/base64"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

// ExternalHashNorm is the Toncenter normalized external-message hash, not the
// hash of the signed body alone. Normalization removes state_init/import_fee
// and stores the body by reference (ton-indexer DataParser.cpp).
func ExternalHashNorm(dst *address.Address, body *cell.Cell) string {
	msg := cell.BeginCell().MustStoreUInt(0b10, 2).MustStoreUInt(0, 2).
		MustStoreAddr(dst).MustStoreCoins(0).MustStoreUInt(0, 1).
		MustStoreUInt(1, 1).MustStoreRef(body).EndCell()
	return base64.StdEncoding.EncodeToString(msg.Hash())
}
