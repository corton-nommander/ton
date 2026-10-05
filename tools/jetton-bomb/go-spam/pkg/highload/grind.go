package highload

import (
	"crypto/ed25519"

	"github.com/xssnick/tonutils-go/address"
)

// Default is four relayers. Testnet can use 16 to spread the dispatch queue across more initiators.
const NumShards = 4
const MaxShards = 16

// Shard returns the address's top-two-bit bucket [0,4), keeping relays intra-shard at split depth <=2.
func Shard(a *address.Address) int {
	return int(a.Data()[0] >> 6)
}

// ShardFor selects the first two bits with four relayers or the first nibble with 16.
func ShardFor(a *address.Address, count int) int {
	if count == MaxShards {
		return int(a.Data()[0] >> 4)
	}
	return Shard(a)
}

// Keep the existing wallets at old nibble buckets 0/4/8/c. Each now serves its entire quarter, so
// narrowing the old 16-wallet set to four never changes these wallets' addresses or needs new keys.
// Grind within the preserved nibble, then route by the first two bits rather than the first four.
func GrindSubwallets(pub ed25519.PublicKey, timeout uint32) [NumShards]uint32 {
	var out [NumShards]uint32
	var found [NumShards]bool
	remaining := NumShards
	for sw := uint32(0); remaining > 0; sw++ {
		w := New(sw, pub, nil, timeout)
		nibble := int(w.addr.Data()[0] >> 4)
		if nibble%4 != 0 {
			continue
		}
		b := nibble / 4
		if !found[b] {
			found[b] = true
			out[b] = sw
			remaining--
		}
	}
	return out
}

// NewShardSet returns the four preserved relayers (indexed by top-two-bit shard bucket) with the
// private key attached, ready to sign relays.
func NewShardSet(pub ed25519.PublicKey, priv ed25519.PrivateKey, timeout uint32) [NumShards]*HighloadWallet {
	subs := GrindSubwallets(pub, timeout)
	var out [NumShards]*HighloadWallet
	for i := 0; i < NumShards; i++ {
		out[i] = New(subs[i], pub, priv, timeout)
	}
	return out
}

// NewShardWallets selects four preserved 0/4/8/c wallets or the original 16-wallet set.
// No addresses change when switching a deployment between the two routing configurations.
func NewShardWallets(pub ed25519.PublicKey, priv ed25519.PrivateKey, timeout uint32, count int) []*HighloadWallet {
	if count != NumShards && count != MaxShards {
		panic("highload: shard count must be 4 or 16")
	}
	if count == NumShards {
		set := NewShardSet(pub, priv, timeout)
		return set[:]
	}
	out := make([]*HighloadWallet, count)
	for sw, remaining := uint32(0), count; remaining > 0; sw++ {
		w := New(sw, pub, priv, timeout)
		i := ShardFor(w.Address(), count)
		if out[i] == nil {
			out[i] = w
			remaining--
		}
	}
	return out
}
