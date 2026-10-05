package statictps

import (
	"bytes"
	"crypto/ed25519"
	"testing"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

func testAddress(wc int32, first byte) *address.Address {
	hash := make([]byte, 32)
	hash[0] = first
	return address.NewAddress(0, byte(wc), hash)
}

func TestStorageParityAndTruncation(t *testing.T) {
	owner := testAddress(-1, 7)
	data := Data(owner, 123)
	s, err := ParseState(data)
	if err != nil {
		t.Fatal(err)
	}
	if s.Owner != owner.StringRaw() || s.Running || s.TargetTPS != 123 || s.TotalTx != 0 || s.Epoch != 0 {
		t.Fatalf("unexpected state %+v", s)
	}
	// An owner and counter prefix alone must never be treated as a valid snapshot.
	if _, err := ParseState(cell.BeginCell().MustStoreAddr(owner).MustStoreUInt(0, 1).EndCell()); err == nil {
		t.Fatal("accepted truncated storage")
	}
	if bytes.Equal(Data(owner, 123).Hash(), Data(owner, 124).Hash()) {
		t.Fatal("address nonce did not alter state")
	}
}

func TestV3SignatureAndLayout(t *testing.T) {
	key := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{0x42}, 32))
	message := cell.BeginCell().MustStoreUInt(0xfeed, 16).EndCell()
	body, err := V3External(19, 42, 12345, key, []*cell.Cell{message})
	if err != nil {
		t.Fatal(err)
	}
	s := body.BeginParse()
	signature, err := s.LoadSlice(512)
	if err != nil {
		t.Fatal(err)
	}
	signed, err := s.ToCell()
	if err != nil {
		t.Fatal(err)
	}
	if !ed25519.Verify(key.Public().(ed25519.PublicKey), signed.Hash(), signature) {
		t.Fatal("invalid V3 signature")
	}
	for _, want := range []uint64{42, 12345, 19} {
		got, err := s.LoadUInt(32)
		if err != nil || got != want {
			t.Fatalf("field got=%d want=%d err=%v", got, want, err)
		}
	}
	mode, err := s.LoadUInt(8)
	if err != nil || mode != 3 {
		t.Fatal("send mode mismatch")
	}
	msg, err := s.LoadRefCell()
	if err != nil || !bytes.Equal(msg.Hash(), message.Hash()) {
		t.Fatal("signed ref mismatch")
	}
	if _, err := V3External(0, 42, 12345, key, make([]*cell.Cell, 5)); err == nil {
		t.Fatal("accepted >4 refs")
	}
}

func TestReceiptCountsIncludeAbortsAndExcludeBounceAsWork(t *testing.T) {
	account := testAddress(-1, 3)
	work := cell.BeginCell().MustStoreUInt(WorkOp, 32).MustStoreUInt(7, 32).EndCell()
	makeTx := func(bounced, aborted bool) *tlb.Transaction {
		tx := &tlb.Transaction{Description: tlb.TransactionDescriptionOrdinary{Aborted: aborted}}
		tx.IO.In = &tlb.Message{MsgType: tlb.MsgTypeInternal, Msg: &tlb.InternalMessage{SrcAddr: account, DstAddr: account, Bounced: bounced, Body: work}}
		return tx
	}
	var counts Counts
	counts.Add(makeTx(false, false), account)
	counts.Add(makeTx(false, true), account)
	counts.Add(makeTx(true, true), account)
	if counts.Transactions != 3 || counts.Internal != 3 || counts.Aborted != 2 || counts.Bounced != 1 || counts.WorkReceipts != 2 || counts.WorkAborted != 1 {
		t.Fatalf("bad accounting %+v", counts)
	}
}
