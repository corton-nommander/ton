package main

import (
	"bytes"
	"crypto/ed25519"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"testing"

	"github.com/victor/jetton-bomb-go-spam/pkg/nativepay"
	"github.com/victor/jetton-bomb-go-spam/pkg/nativerpc"
	"github.com/xssnick/tonutils-go/address"
)

func auditSource(id byte, origin, issued uint64) *source {
	key := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{id}, 32))
	dst := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{id + 1}, 32)).Public().(ed25519.PublicKey)
	return &source{key: key, address: nativepay.AddressFromKey(key), destination: address.NewAddress(0, 0, dst), origin: origin, issued: issued, observed: origin}
}
func cloneSnapshot(s *nativerpc.Snapshot) *nativerpc.Snapshot {
	copy := *s
	copy.Accounts = append([]nativerpc.Account(nil), s.Accounts...)
	return &copy
}
func intervalFixture() ([]*source, *nativerpc.Snapshot, *nativerpc.Snapshot, options) {
	s := auditSource(0x42, 9, 89)
	o := options{Outputs: 16, Amount: 3, Fee: 2}
	// One parent was committed before the measurement anchor; three more
	// parents commit during the interval. Origin intentionally is not modulo16.
	b := &nativerpc.Snapshot{Accounts: []nativerpc.Account{{Address: s.address.StringRaw(), Nonce: 25, Balance: 1000}, {Address: s.destination.StringRaw(), Nonce: 4, Balance: 200}}}
	a := cloneSnapshot(b)
	a.Accounts[0].Nonce = 73
	a.Accounts[0].Balance = 760
	a.Accounts[1].Balance = 344
	return []*source{s}, b, a, o
}

func TestAuditCountsPaymentsAndAtomicParentsSeparately(t *testing.T) {
	s, b, a, o := intervalFixture()
	rows, payments, parents, err := auditInterval(b, a, s, o)
	if err != nil {
		t.Fatal(err)
	}
	if payments != 48 || parents != 3 || len(rows) != 1 || rows[0].CommittedPayments != 48 || rows[0].CompletedParents != 3 || !rows[0].BalanceVerified {
		t.Fatalf("wrong fixed-quantum accounting rows=%+v payments=%d parents=%d", rows, payments, parents)
	}
	rows, payments, parents, err = auditInterval(b, b, s, o)
	if err != nil || payments != 0 || parents != 0 || !rows[0].BalanceVerified {
		t.Fatalf("zero interval failed: rows=%+v err=%v", rows, err)
	}
}

func TestAuditRejectsModuloAndUnrelatedAccountTraffic(t *testing.T) {
	tests := []struct {
		name   string
		mutate func([]*source, *nativerpc.Snapshot, *nativerpc.Snapshot)
	}{
		{"partial atomic parent", func(_ []*source, _ *nativerpc.Snapshot, a *nativerpc.Snapshot) { a.Accounts[0].Nonce = 72 }},
		{"partial parent at start", func(_ []*source, b *nativerpc.Snapshot, _ *nativerpc.Snapshot) { b.Accounts[0].Nonce = 24 }},
		{"nonce backwards", func(_ []*source, b *nativerpc.Snapshot, a *nativerpc.Snapshot) {
			a.Accounts[0].Nonce = b.Accounts[0].Nonce - 16
		}},
		{"nonce before origin", func(_ []*source, b *nativerpc.Snapshot, _ *nativerpc.Snapshot) { b.Accounts[0].Nonce = 8 }},
		{"nonce beyond reservation", func(_ []*source, _ *nativerpc.Snapshot, a *nativerpc.Snapshot) { a.Accounts[0].Nonce = 105 }},
		{"source topped up", func(_ []*source, _ *nativerpc.Snapshot, a *nativerpc.Snapshot) { a.Accounts[0].Balance++ }},
		{"destination extra incoming payment", func(_ []*source, _ *nativerpc.Snapshot, a *nativerpc.Snapshot) { a.Accounts[1].Balance++ }},
		{"destination paid somebody else", func(_ []*source, _ *nativerpc.Snapshot, a *nativerpc.Snapshot) { a.Accounts[1].Nonce++ }},
		{"source flags changed", func(_ []*source, _ *nativerpc.Snapshot, a *nativerpc.Snapshot) { a.Accounts[0].Flags++ }},
		{"destination flags changed", func(_ []*source, _ *nativerpc.Snapshot, a *nativerpc.Snapshot) { a.Accounts[1].Flags++ }},
		{"wrong source address", func(_ []*source, _ *nativerpc.Snapshot, a *nativerpc.Snapshot) {
			a.Accounts[0].Address = a.Accounts[1].Address
		}},
		{"wrong destination address", func(_ []*source, _ *nativerpc.Snapshot, a *nativerpc.Snapshot) {
			a.Accounts[1].Address = a.Accounts[0].Address
		}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			s, b, a, o := intervalFixture()
			test.mutate(s, b, a)
			if _, _, _, err := auditInterval(b, a, s, o); err == nil {
				t.Fatal("invalid cohort accepted")
			}
		})
	}
}

func TestAuditRejectsIncompleteSnapshotsAndArithmeticOverflow(t *testing.T) {
	s, b, a, o := intervalFixture()
	if _, _, _, err := auditInterval(nil, a, s, o); err == nil {
		t.Fatal("nil snapshot accepted")
	}
	if _, _, _, err := auditInterval(b, &nativerpc.Snapshot{}, s, o); err == nil {
		t.Fatal("incomplete snapshot accepted")
	}
	// Multiplying logical payments by per-payment debit requires big.Int;
	// representable balances cannot satisfy this intentionally larger product.
	o.Amount = math.MaxUint64 / 2
	o.Fee = 0
	if _, _, _, err := auditInterval(b, a, s, o); err == nil {
		t.Fatal("overflowing debit product accepted")
	}
	// Amount+fee must be rejected before uint64 addition can wrap to zero.
	s, b, a, o = intervalFixture()
	o.Amount = math.MaxUint64
	o.Fee = 1
	a.Accounts[0].Balance = b.Accounts[0].Balance
	a.Accounts[1].Balance = b.Accounts[1].Balance
	if _, _, _, err := auditInterval(b, a, s, o); err == nil {
		t.Fatal("amount plus fee overflow accepted")
	}
	// Two individually valid source intervals can overflow the aggregate count.
	// Each pair uses one nanogram payments, so exact uint64 balance conservation
	// remains possible and cannot substitute for checked aggregate addition.
	first, second := auditSource(0x22, 0, math.MaxUint64-1), auditSource(0x32, 0, math.MaxUint64-1)
	before := &nativerpc.Snapshot{Accounts: []nativerpc.Account{{Address: first.address.StringRaw(), Balance: math.MaxUint64 - 1}, {Address: second.address.StringRaw(), Balance: math.MaxUint64 - 1}, {Address: first.destination.StringRaw()}, {Address: second.destination.StringRaw()}}}
	after := cloneSnapshot(before)
	for i := 0; i < 2; i++ {
		after.Accounts[i].Nonce = math.MaxUint64 - 1
		after.Accounts[i].Balance = 0
		after.Accounts[i+2].Balance = math.MaxUint64 - 1
	}
	if _, _, _, err := auditInterval(before, after, []*source{first, second}, options{Outputs: 1, Amount: 1}); err == nil {
		t.Fatal("aggregate payments/parents count overflow accepted")
	}
}

func TestApplyObservedRetiresOnlyConfirmedWholeParents(t *testing.T) {
	s := auditSource(0x42, 9, 57)
	p0, p1, p2 := &parent{nonce: 9}, &parent{nonce: 25}, &parent{nonce: 41}
	s.pending = []*parent{p0, p1, p2}
	snap := &nativerpc.Snapshot{Accounts: []nativerpc.Account{{Address: s.address.StringRaw(), Nonce: 25}}}
	if err := applyObserved([]*source{s}, snap, 16); err != nil {
		t.Fatal(err)
	}
	if s.observed != 25 || len(s.pending) != 2 || s.pending[0] != p1 || s.pending[1] != p2 {
		t.Fatal("retired wrong parent interval")
	}
	if err := applyObserved([]*source{s}, snap, 16); err != nil || len(s.pending) != 2 {
		t.Fatal("duplicate canonical anchor changed pending parents")
	}
	snap.Accounts[0].Nonce = 57
	if err := applyObserved([]*source{s}, snap, 16); err != nil || len(s.pending) != 0 {
		t.Fatal("completed atomic parents not retired")
	}
}

func TestApplyObservedRejectsInvalidSnapshotsWithoutPanic(t *testing.T) {
	tests := []struct {
		name    string
		snap    *nativerpc.Snapshot
		outputs int
	}{
		{"nil", nil, 16},
		{"missing accounts", &nativerpc.Snapshot{}, 16},
		{"wrong identity", &nativerpc.Snapshot{Accounts: []nativerpc.Account{{Address: "0:wrong", Nonce: 25}}}, 16},
		{"partial parent", &nativerpc.Snapshot{Accounts: []nativerpc.Account{{Nonce: 24}}}, 16},
		{"before origin", &nativerpc.Snapshot{Accounts: []nativerpc.Account{{Nonce: 8}}}, 16},
		{"beyond reservation", &nativerpc.Snapshot{Accounts: []nativerpc.Account{{Nonce: 73}}}, 16},
		{"zero quantum", &nativerpc.Snapshot{Accounts: []nativerpc.Account{{Nonce: 25}}}, 0},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			defer func() {
				if r := recover(); r != nil {
					t.Fatalf("malformed observation panicked: %v", r)
				}
			}()
			s := auditSource(0x42, 9, 57)
			if test.snap != nil && len(test.snap.Accounts) > 0 && test.name != "wrong identity" {
				test.snap.Accounts[0].Address = s.address.StringRaw()
			}
			if err := applyObserved([]*source{s}, test.snap, test.outputs); err == nil {
				t.Fatal("invalid observation accepted")
			}
			if s.observed != 9 {
				t.Fatal("invalid observation consumed source state")
			}
		})
	}
}

func writeWallet(t *testing.T, dir string, index int, seed, pub, dst []byte) {
	t.Helper()
	for name, data := range map[string][]byte{"source-%d.pk": seed, "source-%d.pub": pub, "dest-%d.pub": dst} {
		if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf(name, index)), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
}
func TestLoadSourcesRawKeysAndDistinctAuditPairs(t *testing.T) {
	seed := bytes.Repeat([]byte{0x42}, 32)
	key := ed25519.NewKeyFromSeed(seed)
	pub := key.Public().(ed25519.PublicKey)
	dst := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{0x43}, 32)).Public().(ed25519.PublicKey)
	dir := t.TempDir()
	writeWallet(t, dir, 0, seed, pub, dst)
	s, err := loadSources(dir, 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(s) != 1 || !bytes.Equal(s[0].key, key) || !bytes.Equal(s[0].address.Data(), pub) || !bytes.Equal(s[0].destination.Data(), dst) {
		t.Fatal("wrong raw key/account derivation")
	}
	tests := []struct {
		name           string
		seed, pub, dst []byte
	}{
		{"short seed", seed[:31], pub, dst},
		{"short public key", seed, pub[:31], dst},
		{"short destination", seed, pub, dst[:31]},
		{"seed public mismatch", seed, dst, pub},
		{"self destination", seed, pub, pub},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			dir := t.TempDir()
			writeWallet(t, dir, 0, test.seed, test.pub, test.dst)
			if _, err := loadSources(dir, 1); err == nil {
				t.Fatal("invalid wallet material accepted")
			}
		})
	}
	// A reused key or a destination also acting as a source invalidates the
	// independent debit/credit audit even when each file is individually valid.
	writeWallet(t, dir, 1, seed, pub, dst)
	if _, err := loadSources(dir, 2); err == nil {
		t.Fatal("duplicate pair accepted")
	}
	seed2 := bytes.Repeat([]byte{0x43}, 32)
	pub2 := ed25519.NewKeyFromSeed(seed2).Public().(ed25519.PublicKey)
	dst2 := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{0x44}, 32)).Public().(ed25519.PublicKey)
	writeWallet(t, dir, 1, seed2, pub2, dst2)
	if _, err := loadSources(dir, 2); err == nil {
		t.Fatal("cross-pair source/destination collision accepted")
	}
}
