package fatpool

import (
	"bufio"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/xssnick/tonutils-go/address"
)

// dumpDir is the original per-pool address dump produced by jetton-prepaid/scripts/dump-fat-addrs.tolk.
// It is gitignored (37 MB), which is exactly why this package derives instead of storing — so the test
// skips when it is absent and acts as a local proof when it is present.
const dumpDir = "../../../jetton-prepaid/fats-160"

// TestDeriveMatchesDump is the ground truth for the whole package: every address the bot will send to must
// equal the address the deploy actually created. It replays all 781,000 dumped addresses against Derive.
//
// Compare RAW (workchain + 32-byte account id), never the user-friendly string: the dump was written with
// the testnet flag (kQ…) while address.CalcAddress returns the mainnet form (EQ…). Same account, different
// display flag — comparing strings reports 781k false mismatches.
func TestDeriveMatchesDump(t *testing.T) {
	if _, err := os.Stat(dumpDir); err != nil {
		t.Skipf("no %s (gitignored dump) — skipping ground-truth check", dumpDir)
	}
	code, err := Code()
	if err != nil {
		t.Fatalf("code: %v", err)
	}

	total := 0
	for _, p := range Pools {
		path := filepath.Join(dumpDir, p.Name+".addrs")
		f, err := os.Open(path)
		if err != nil {
			t.Errorf("pool %s: %v", p.Name, err)
			continue
		}

		pub := mustPub(t, p.PubHex)
		sc := bufio.NewScanner(f)
		sc.Buffer(make([]byte, 1<<20), 1<<20)
		i := 0
		for sc.Scan() {
			fields := strings.Fields(sc.Text())
			if len(fields) < 2 || fields[0] != "ADDR" {
				continue
			}
			want, err := address.ParseAddr(fields[1])
			if err != nil {
				t.Fatalf("pool %s line %d: %v", p.Name, i+1, err)
			}
			got := Address(code, uint32(i), pub)
			if got.Workchain() != want.Workchain() || string(got.Data()) != string(want.Data()) {
				t.Fatalf("pool %s index %d: derived %s, dump has %s", p.Name, i, got.StringRaw(), want.StringRaw())
			}
			i++
		}
		f.Close()
		if err := sc.Err(); err != nil {
			t.Fatalf("pool %s: scan: %v", p.Name, err)
		}
		if i != p.Count {
			t.Errorf("pool %s: manifest says %d accounts, dump has %d", p.Name, p.Count, i)
		}
		total += i
	}
	t.Logf("verified %d derived addresses against the dump", total)
}

// TestSizeInventory pins the deployed tiers so a manifest edit that drops or renames a tier is loud.
func TestSizeInventory(t *testing.T) {
	want := map[int]int{10: 100000, 25: 100000, 50: 100000, 100: 431000, 200: 50000}
	sizes := Sizes()
	if len(sizes) != len(want) {
		t.Fatalf("sizes %v, want %d tiers", sizes, len(want))
	}
	for _, s := range sizes {
		if got := CountFor(s); got != want[s] {
			t.Errorf("size %dKB: %d accounts, want %d", s, got, want[s])
		}
	}
	if HasSize(42) {
		t.Error("HasSize(42) must be false")
	}
}

// TestDeriveIsDeterministic guards the derivation itself: same tier, same addresses, same order — the bot
// picks recipients by index, so a reordering would silently change which accounts a run touches.
func TestDeriveIsDeterministic(t *testing.T) {
	a, err := Derive(200)
	if err != nil {
		t.Fatalf("derive: %v", err)
	}
	b, err := Derive(200)
	if err != nil {
		t.Fatalf("derive: %v", err)
	}
	if len(a) != CountFor(200) {
		t.Fatalf("derived %d addresses, want %d", len(a), CountFor(200))
	}
	for i := range a {
		if a[i].String() != b[i].String() {
			t.Fatalf("index %d differs between runs: %s vs %s", i, a[i], b[i])
		}
	}
	if _, err := Derive(42); err == nil {
		t.Error("Derive(42) must fail — no such tier")
	}
}

func mustPub(t *testing.T, hexKey string) []byte {
	t.Helper()
	b, err := hex.DecodeString(hexKey)
	if err != nil {
		t.Fatalf("bad pubkey hex %q: %v", hexKey, err)
	}
	if len(b) != 32 {
		t.Fatalf("pubkey %q is %d bytes, want 32", hexKey, len(b))
	}
	return b
}
