// Package nativepay constructs the sidechain's native payment authorizations.
// These are raw NTXF/NTRN cells, not TON external-in messages to a TVM wallet.
// The codec mirrors crypto/block/transaction.cpp and tonlib/native-client.cpp.
package nativepay

import (
	"bytes"
	"crypto/ed25519"
	"encoding/binary"
	"fmt"
	"math"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

const (
	TransferMagic    = 0x4e545846 // NTXF: one signed payment.
	RunMagic         = 0x4e54524e // NTRN: one signature covering a contiguous nonce run.
	OutputsLeafMagic = 0x4e54524c // NTRL.
	OutputsNodeMagic = 0x4e545244 // NTRD.
	MaxOutputs       = 16
	MaxLaneDepth     = 60
)

// Output contains protocol uint64 nanoTON/GRAM amounts, avoiding floating point.
type Output struct {
	Destination *address.Address
	Amount      uint64
	Fee         uint64
}

// AddressFromKey derives the balance-only basechain account whose id is the
// Ed25519 public key. Invalid key lengths return nil.
func AddressFromKey(key ed25519.PrivateKey) *address.Address {
	if len(key) != ed25519.PrivateKeySize {
		return nil
	}
	return address.NewAddress(0, 0, key.Public().(ed25519.PublicKey))
}

// BuildMessage returns a raw signed payment root for liteServer.sendMessage or
// sendMessageBatch. Never wrap it in tlb.ExternalMessage. chainDomain must be
// the immutable masterchain zerostate root hash. signedRun chooses NTRN (1..16
// outputs); scalar NTXF accepts one output and is rejected by run-only chains.
// laneDepth=0 leaves locality checking to admission; otherwise all destinations
// must share the source's high laneDepth address bits. Each output consumes one
// nonce beginning at firstNonce, including a final increment of the account
// nonce. No state lookup, funding, or nonce reservation occurs here.
func BuildMessage(key ed25519.PrivateKey, outputs []Output, firstNonce uint64, validUntil uint32, chainDomain []byte, signedRun bool, laneDepth uint32) (*cell.Cell, error) {
	if err := validate(key, outputs, firstNonce, validUntil, chainDomain, signedRun, laneDepth); err != nil {
		return nil, err
	}
	source := AddressFromKey(key)
	payload := signingPayload(source, outputs, firstNonce, validUntil, chainDomain, signedRun)
	signature := cell.BeginCell().MustStoreSlice(ed25519.Sign(key, payload), 512).EndCell()
	if !signedRun {
		output := outputs[0]
		return cell.BeginCell().MustStoreUInt(TransferMagic, 32).
			MustStoreSlice(source.Data(), 256).MustStoreSlice(output.Destination.Data(), 256).
			MustStoreUInt(output.Amount, 64).MustStoreUInt(output.Fee, 64).
			MustStoreUInt(firstNonce, 64).MustStoreUInt(uint64(validUntil), 32).
			MustStoreRef(signature).EndCell(), nil
	}
	return cell.BeginCell().MustStoreUInt(RunMagic, 32).MustStoreSlice(source.Data(), 256).
		MustStoreUInt(firstNonce, 64).MustStoreUInt(uint64(validUntil), 32).
		MustStoreUInt(uint64(len(outputs)), 5).MustStoreRef(signature).
		MustStoreRef(outputTree(outputs)).EndCell(), nil
}

func validate(key ed25519.PrivateKey, outputs []Output, firstNonce uint64, validUntil uint32, domain []byte, signedRun bool, laneDepth uint32) error {
	if len(key) != ed25519.PrivateKeySize {
		return fmt.Errorf("native payment requires a 64-byte ed25519 private key derived from a 32-byte seed")
	}
	if !bytes.Equal(key, ed25519.NewKeyFromSeed(key.Seed())) {
		return fmt.Errorf("ed25519 private key public half does not match its seed")
	}
	if len(domain) != 32 {
		return fmt.Errorf("chain domain must be the 32-byte masterchain zerostate root hash")
	}
	if len(outputs) < 1 || len(outputs) > MaxOutputs || (!signedRun && len(outputs) != 1) {
		return fmt.Errorf("scalar payment requires one output; signed run requires 1..16 outputs")
	}
	if firstNonce > math.MaxUint64-uint64(len(outputs)) {
		return fmt.Errorf("payment nonce interval overflows uint64 after the final increment")
	}
	if validUntil == 0 {
		return fmt.Errorf("valid_until must be positive")
	}
	if laneDepth > MaxLaneDepth {
		return fmt.Errorf("lane depth must be 0..60")
	}
	source := AddressFromKey(key)
	var total uint64
	for i, output := range outputs {
		if output.Destination == nil || output.Destination.Workchain() != 0 || len(output.Destination.Data()) != 32 {
			return fmt.Errorf("output %d destination must be a standard basechain account", i)
		}
		if output.Amount == 0 || output.Fee > math.MaxUint64-output.Amount {
			return fmt.Errorf("output %d amount must be positive and amount plus fee must fit uint64", i)
		}
		debit := output.Amount + output.Fee
		if debit > math.MaxUint64-total {
			return fmt.Errorf("aggregate payment debit overflows uint64")
		}
		total += debit
		if laneDepth > 0 && !sameLane(source.Data(), output.Destination.Data(), laneDepth) {
			return fmt.Errorf("output %d destination is outside the source payment lane", i)
		}
	}
	return nil
}

func sameLane(source, destination []byte, depth uint32) bool {
	whole, remainder := depth/8, depth%8
	if !bytes.Equal(source[:whole], destination[:whole]) {
		return false
	}
	if remainder == 0 {
		return true
	}
	mask := byte(0xff << (8 - remainder))
	return source[whole]&mask == destination[whole]&mask
}

func append32(dst []byte, value uint32) []byte { return binary.BigEndian.AppendUint32(dst, value) }
func append64(dst []byte, value uint64) []byte { return binary.BigEndian.AppendUint64(dst, value) }

// Wire count is five bits, whereas the signature payload count is one byte.
// Sign the complete field bytes directly, not their SHA-256/cell hash.
func signingPayload(source *address.Address, outputs []Output, nonce uint64, until uint32, domain []byte, run bool) []byte {
	separator := "TON_NATIVE_TRANSFER_V1"
	if run {
		separator = "TON_NATIVE_TRANSFER_RUN_V1"
	}
	payload := append([]byte(separator), domain...)
	if !run {
		output := outputs[0]
		payload = append32(payload, TransferMagic)
		payload = append(payload, source.Data()...)
		payload = append(payload, output.Destination.Data()...)
		payload = append64(payload, output.Amount)
		payload = append64(payload, output.Fee)
		payload = append64(payload, nonce)
		return append32(payload, until)
	}
	payload = append32(payload, RunMagic)
	payload = append(payload, source.Data()...)
	payload = append64(payload, nonce)
	payload = append32(payload, until)
	payload = append(payload, byte(len(outputs)))
	for _, output := range outputs {
		payload = append(payload, output.Destination.Data()...)
		payload = append64(payload, output.Amount)
		payload = append64(payload, output.Fee)
	}
	return payload
}

// Canonical ordered tree: adjacent output pairs form leaves; adjacent nodes
// form each next level. An odd final node is promoted unchanged. Tree shape is
// consensus relevant and a field-equivalent alternate shape is rejected.
func outputTree(outputs []Output) *cell.Cell {
	type node struct {
		root  *cell.Cell
		count uint64
	}
	level := make([]node, 0, (len(outputs)+1)/2)
	for start := 0; start < len(outputs); start += 2 {
		end := start + 2
		if end > len(outputs) {
			end = len(outputs)
		}
		b := cell.BeginCell().MustStoreUInt(OutputsLeafMagic, 32).MustStoreUInt(uint64(end-start), 5)
		for _, output := range outputs[start:end] {
			b.MustStoreSlice(output.Destination.Data(), 256).MustStoreUInt(output.Amount, 64).MustStoreUInt(output.Fee, 64)
		}
		level = append(level, node{b.EndCell(), uint64(end - start)})
	}
	for len(level) > 1 {
		next := make([]node, 0, (len(level)+1)/2)
		for i := 0; i < len(level); i += 2 {
			if i+1 == len(level) {
				next = append(next, level[i])
				continue
			}
			left, right := level[i], level[i+1]
			next = append(next, node{cell.BeginCell().MustStoreUInt(OutputsNodeMagic, 32).MustStoreUInt(left.count, 5).MustStoreRef(left.root).MustStoreRef(right.root).EndCell(), left.count + right.count})
		}
		level = next
	}
	return level[0].root
}
