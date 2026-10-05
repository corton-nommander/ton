package main

import (
	"bytes"
	"context"
	"encoding/hex"
	"strings"
	"testing"

	"github.com/victor/jetton-bomb-go-spam/pkg/statictps"
	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/ton"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

type historyAPI struct {
	ton.APIClientWrapped
	pages [][]*tlb.Transaction
	calls int
}

func (a *historyAPI) ListTransactions(_ context.Context, _ *address.Address, _ uint32, _ uint64, _ []byte) ([]*tlb.Transaction, error) {
	page := a.pages[a.calls]
	a.calls++
	return page, nil
}

func TestCLIRequiresExplicitWorkchainAndBounds(t *testing.T) {
	base := []string{"plan", "--ls-config", "local.json", "--owner", "-1:" + strings.Repeat("00", 32)}
	if _, err := parse(base); err == nil {
		t.Fatal("missing explicit workchain accepted")
	}
	valid := append(append([]string{}, base...), "--workchain", "-1")
	if _, err := parse(valid); err != nil {
		t.Fatal(err)
	}
	if _, err := parse(append(valid, "--contracts", "257")); err == nil {
		t.Fatal("unbounded contract count accepted")
	}
}

func historyFixture() (*address.Address, *snapshot, *snapshot, []*tlb.Transaction) {
	a := address.NewAddress(0, 255, bytes.Repeat([]byte{7}, 32))
	txs := make([]*tlb.Transaction, 3)
	for i := range txs {
		tx := &tlb.Transaction{AccountAddr: a.Data(), LT: uint64(i + 1), Hash: bytes.Repeat([]byte{byte(i + 1)}, 32), PrevTxLT: uint64(i), PrevTxHash: bytes.Repeat([]byte{byte(i)}, 32), Description: tlb.TransactionDescriptionOrdinary{Aborted: i == 2}}
		tx.IO.In = &tlb.Message{MsgType: tlb.MsgTypeInternal, Msg: &tlb.InternalMessage{SrcAddr: a, DstAddr: a, Body: cell.BeginCell().MustStoreUInt(statictps.WorkOp, 32).MustStoreUInt(0, 32).EndCell()}}
		txs[i] = tx
	}
	start := &snapshot{Accounts: []accountSnapshot{{LastLT: 1, LastHash: hex.EncodeToString(txs[0].Hash)}}}
	end := &snapshot{Accounts: []accountSnapshot{{LastLT: 3, LastHash: hex.EncodeToString(txs[2].Hash)}}}
	return a, start, end, txs
}

func TestAuditExactAnchorsAndAbortedReceipts(t *testing.T) {
	a, start, end, txs := historyFixture()
	api := &historyAPI{pages: [][]*tlb.Transaction{txs}}
	counts, err := audit(context.Background(), api, []contract{{addr: a}}, start, end, 100)
	if err != nil {
		t.Fatal(err)
	}
	if counts.Internal != 2 || counts.WorkReceipts != 2 || counts.Aborted != 1 {
		t.Fatalf("start anchor was counted or aborted receipt lost: %+v", counts)
	}
}

func TestAuditRejectsForkAndCap(t *testing.T) {
	a, start, end, txs := historyFixture()
	start.Accounts[0].LastHash = hex.EncodeToString(bytes.Repeat([]byte{9}, 32))
	if _, err := audit(context.Background(), &historyAPI{pages: [][]*tlb.Transaction{txs}}, []contract{{addr: a}}, start, end, 100); err == nil {
		t.Fatal("forked start anchor accepted")
	}
	a, start, end, txs = historyFixture()
	if _, err := audit(context.Background(), &historyAPI{pages: [][]*tlb.Transaction{txs}}, []contract{{addr: a}}, start, end, 1); err == nil {
		t.Fatal("incomplete cap-truncated audit accepted")
	}
}
