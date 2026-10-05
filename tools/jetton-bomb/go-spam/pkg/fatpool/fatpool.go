// Package fatpool derives the addresses of the FatReproduce "fat" load-target accounts that inflate the
// controlled testnet's state (10/25/50/100/200 KB per account, 781k accounts total, ~160 GB).
//
// The addresses are COMPUTED, not stored. A FatReproduce address is a pure state_init hash over
// (code, subwalletId, publicKey) — no minter, no size — so the whole set is reproduced from a 19-line
// manifest of (pool pubkey, count, size) instead of a 37 MB address dump. The pools exist because each
// size tier was deployed by its own key (sizes don't change the address, only the key does).
//
// Storage preimage (must match FatReproduceStorage.toCell in contracts/FatReproduce.tolk):
//
//	seqno:uint32=0 | subwalletId:uint32 | publicKey:uint256 | cellsBuilt:uint32=0 | batches:dict=empty
//
// Verified: derive_test.go reproduces all 781,000 addresses from jetton-prepaid/fats-160/*.addrs exactly.
package fatpool

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"sort"
	"sync"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

// CodeBase64 is the compiled FatReproduce code (jetton-prepaid/build/FatReproduce.json), representation
// hash 1D7C6F8FC61EF0B3994D7B03DEFB8D3FDC5AC67B2266F443991136C58E6E1FD1. Part of every fat address, so
// recompiling the contract invalidates the whole set.
const CodeBase64 = "" +
	"te6ccgECFwEAA9kAART/APSkE/S88sgLAQIBIAIDAgFIBAUAoPLwAYMI1xgg0x/TH9Mf7UTQA/gjvPKjAtMfINMf1wv/UUO68qEV" +
	"uvKiBMjO+RZAVfkQ8qT4AAGkyMsfzsntVPgPlSDXSsIAltMH1AL7AOgwAgLPBgcCASAREgT3NcsJ9PwaoTjAtcsJ9PYCITjAtcsJ" +
	"9PvKISO4O1E0NMfMdMf1wv/+JIipfgqbcjPkAAAAAITyx8kzwv/z5AAAAACEvQAyQHIz4TQzMz5FsjPigBAy//PUMcF8uGRAtMf+" +
	"kgwUyG+jhJsMcjPhQj6UnDPC27JgQCi+wDjDoAgJCgsAJyB/3f4MyBukTDg0NcLD8MB8uKagAf7wAe1E0AHTH9Mf0w/6ANMf0w/6" +
	"ADAH0x8x0x/XC//4kvgqbcjPkAAAAAIayx8jzwv/z5AAAAACGfQAyVAIyM+E0MzM+RbIz4oAQMv/z1AXxwXy4ZH4KMjPhQj6Uij6" +
	"AoIQ+nsBEM8LiiPPCx8izwsPKPoCyXH7AFMGvpJfCOAkDAHY8AHtRND4kvgoxwXy4ZEB0x/TD/oAMFRyIbySMCHeUgXWH9Mf0//T" +
	"H/QFkynCAIroOQPIzhLLH8v/yx8V9ADJ7VRQI6EgwgCOH/goyM+FCPpSI/oCghD6ewEQzwuKyx/LDwH6Aslx+wCSXwPiDwCQAqT4" +
	"Km3Iz5AAAAACE8sfFcv/z5AAAAAC9ADJUAPIz4TQzMz5FsjPigBAy//PUMjPhQj6UoIQ+n3lEM8LjhLLH/pSyYEAovsAAATgMAE8" +
	"wgCXJIEA/bvDAJFw4vKmI3L7AlNgoXB/kSCK6F8LDQH+UzGgpHAocQWkUwu+kX+VUwa+wwDimzRbMnAqcIMGBQME3vgqbcjPkAAA" +
	"AAIWyx8uzwv/z5AAAAACFfQAyVR4y1PcVhTIz4mIAVN7yM+E0MzM+RbPC/9QCfoCgQCMzwtwGswVzM+T6fg1QhfLHxLLH8sPUAT6" +
	"AhPLHxPLD1j6Ag4ACslQA/sAAfRTmYEAlryUMIEAlt6IU0SrP1IQsiGrf7IBq7+yhD+wJHCTUwS5jkUoqh8isSOyhD+wKcjLHyPP" +
	"Cx9wkyDBDo4hAoIwWFH0LUyVfy2ogjAUBXt+92eBT6CEP7BSAss/AqQS6DAxFMzJAaQDpBPoMDFBQ4Ag9BdQoqFQmRAAAAIBIBMU" +
	"AgEgFRYAKbs5ztRNDTHzHTHzHT/9MfMfQEMdGAApuMl+1E0NMf0x8x0/8x0x8x9AQx0YACm5PR7UTQ0x8x0x8x0/8x0x/0BDHRgA" +
	"Kbu7vtRNDTHzHTH9P/MdMfMfQEMdGA=="

// Pool is one FatReproduce deploy batch: subwallet ids [0, Count) under PubHex, all built to SizeKB.
type Pool struct {
	Name   string
	SizeKB int
	Count  int
	PubHex string
}

// Pools is the deployed fat inventory of the 160 GB testnet. Counts and keys come from the original
// deploy runs (jetton-prepaid/fat-<name>.key + fats-160/<name>.addrs); the public keys are safe to commit
// — they are already in every fat account's on-chain data.
var Pools = []Pool{
	{Name: "fat-c-100a", SizeKB: 100, Count: 40000, PubHex: "dff7ae342d03c2c39cac34074d5c98e86283ee0ec4501b6d967e9ddd1d218d07"},
	{Name: "fat-c-100b", SizeKB: 100, Count: 40000, PubHex: "53c6c5c084b9eda70067ce924ccc4a012330d094f8f6653c735fd5a4180da29d"},
	{Name: "fat-c-100c", SizeKB: 100, Count: 40000, PubHex: "1ec13a3b410ac276cb1aa4ad70695949da8dba7f7fbb2442850e2679761dace1"},
	{Name: "fat-c-100d", SizeKB: 100, Count: 40000, PubHex: "17d28c6d9e36051c59a7b388a6a39b4de777d5bb4577008e93d743ad9951c0f0"},
	{Name: "fat-c-100e", SizeKB: 100, Count: 40000, PubHex: "989a258c236dd21d689f502f0d7d42988a61e0d3ede79b37171c14bb066d6207"},
	{Name: "fat-c-100f", SizeKB: 100, Count: 40000, PubHex: "7da4f9cc08ac7532edb627133d0de31498518b06eee05f97318e351605984685"},
	{Name: "fat-c-100g", SizeKB: 100, Count: 40000, PubHex: "3d79a33adb67cb465336f78d33bb0ddc01cb444067038d846312d447b1def2ee"},
	{Name: "fat-c-100h", SizeKB: 100, Count: 40000, PubHex: "295b8cd771e23fe3331d8edc29c4e2e230f8d33755775a0eee6e12916b3d7f62"},
	{Name: "fat-c-10a", SizeKB: 10, Count: 50000, PubHex: "f81fc7cf5feee94a8c08475d3c357d04a6b33f875354ed5f95afe32864b96458"},
	{Name: "fat-c-10b", SizeKB: 10, Count: 50000, PubHex: "7c08412db85f50f0a139e4ef005f422300066ee48636b17a910a913fa204d3db"},
	{Name: "fat-c-200a", SizeKB: 200, Count: 25000, PubHex: "bc50a8edd1ccd84c4741090751e50d011297b991b4ca061dc24857974deeea91"},
	{Name: "fat-c-200b", SizeKB: 200, Count: 25000, PubHex: "519b639398f0d806999ab71ec4bde956cfcfd04c2cc0dfef85b403bb0e8e4529"},
	{Name: "fat-c-25a", SizeKB: 25, Count: 50000, PubHex: "b52696b1f11050d53a549fd6389700fa40f36ac6da41052b7e26012b5d80c8ae"},
	{Name: "fat-c-25b", SizeKB: 25, Count: 50000, PubHex: "a6f0a8bd0d4e207f01e0acd45b83731aebb715b65c247b6d799716e6f45ef24a"},
	{Name: "fat-c-50a", SizeKB: 50, Count: 50000, PubHex: "3c859b85eebfe9d5f4d743eb06d222aab291e2df06e64e0bc20a17e55c3b6568"},
	{Name: "fat-c-50b", SizeKB: 50, Count: 50000, PubHex: "395173b09ba077d2be69adb2ee48156d4d0b807820acb0b3b474d7bc0721f54d"},
	{Name: "fat-d-100a", SizeKB: 100, Count: 37000, PubHex: "e16f2918fcbc7616e4191c04252729bf9ba8e97c365a7ddcb8f56c370edb6b36"},
	{Name: "fat-d-100b", SizeKB: 100, Count: 37000, PubHex: "b9e5fc408bbe493a8c0b6d377b4b1ac0766447ee995354cd60c380a4bc6b5d10"},
	{Name: "fat-d-100c", SizeKB: 100, Count: 37000, PubHex: "bac0176ede23b5cf238ce5cc2698429fd421d55c571f641166bb59700790b8cf"},
}

// Sizes lists the deployed size tiers (KB per account), ascending. These are the valid /fat_size values.
func Sizes() []int {
	seen := map[int]bool{}
	var out []int
	for _, p := range Pools {
		if !seen[p.SizeKB] {
			seen[p.SizeKB] = true
			out = append(out, p.SizeKB)
		}
	}
	sort.Ints(out)
	return out
}

// HasSize reports whether a size tier was deployed.
func HasSize(sizeKB int) bool {
	for _, p := range Pools {
		if p.SizeKB == sizeKB {
			return true
		}
	}
	return false
}

// CountFor returns how many fat accounts exist in a size tier.
func CountFor(sizeKB int) int {
	n := 0
	for _, p := range Pools {
		if p.SizeKB == sizeKB {
			n += p.Count
		}
	}
	return n
}

var (
	codeOnce sync.Once
	codeCell *cell.Cell
	codeErr  error
)

// Code returns the parsed FatReproduce code cell (parsed once).
func Code() (*cell.Cell, error) {
	codeOnce.Do(func() {
		raw, err := base64.StdEncoding.DecodeString(CodeBase64)
		if err != nil {
			codeErr = fmt.Errorf("fatpool: decode code: %w", err)
			return
		}
		codeCell, codeErr = cell.FromBOC(raw)
	})
	return codeCell, codeErr
}

// Address derives fat account #subwallet of the pool owning pub. The preimage is the INITIAL storage
// (seqno 0, cellsBuilt 0, empty batch dict) — the account keeps its address as it grows.
func Address(code *cell.Cell, subwallet uint32, pub ed25519.PublicKey) *address.Address {
	data := cell.BeginCell().
		MustStoreUInt(0, 32).                 // seqno
		MustStoreUInt(uint64(subwallet), 32). // subwalletId
		MustStoreSlice(pub, 256).             // publicKey
		MustStoreUInt(0, 32).                 // cellsBuilt
		MustStoreUInt(0, 1).                  // batches: empty dict
		EndCell()
	si := tlb.StateInit{Code: code, Data: data}
	return si.CalcAddress(0)
}

// Derive returns every fat address of the given size tier, in manifest order. It is CPU-bound (two SHA256
// per address: ~430k addresses for the 100 KB tier take a couple of seconds), so callers should derive
// once and cache — see the bot's fatAddrs.
func Derive(sizeKB int) ([]*address.Address, error) {
	if !HasSize(sizeKB) {
		return nil, fmt.Errorf("fatpool: no fat pool of size %d KB (have %v)", sizeKB, Sizes())
	}
	code, err := Code()
	if err != nil {
		return nil, err
	}
	out := make([]*address.Address, 0, CountFor(sizeKB))
	for _, p := range Pools {
		if p.SizeKB != sizeKB {
			continue
		}
		pubBytes, err := hex.DecodeString(p.PubHex)
		if err != nil {
			return nil, fmt.Errorf("fatpool: pool %s bad pubkey: %w", p.Name, err)
		}
		if len(pubBytes) != ed25519.PublicKeySize {
			return nil, fmt.Errorf("fatpool: pool %s pubkey is %d bytes, want 32", p.Name, len(pubBytes))
		}
		pub := ed25519.PublicKey(pubBytes)
		for i := 0; i < p.Count; i++ {
			out = append(out, Address(code, uint32(i), pub))
		}
	}
	return out, nil
}
