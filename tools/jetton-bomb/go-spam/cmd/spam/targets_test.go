package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadTargets(t *testing.T) {
	// Uppercase hex (as bigstate writes), a masterchain addr, a comment, a blank line, and a duplicate.
	content := `# big-state accounts
0:69BA760E445A30CDF6CDC5974BC0FD2C473391EF4E49ACE0C286447DC8EC4806

-1:5555555555555555555555555555555555555555555555555555555555555555
0:69BA760E445A30CDF6CDC5974BC0FD2C473391EF4E49ACE0C286447DC8EC4806
`
	dir := t.TempDir()
	path := filepath.Join(dir, "t.txt")
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}

	got, err := loadTargets(path)
	if err != nil {
		t.Fatalf("loadTargets: %v", err)
	}
	if len(got) != 2 { // duplicate collapsed, comment + blank skipped
		t.Fatalf("got %d addresses, want 2", len(got))
	}
	if got[0].Workchain() != 0 || got[1].Workchain() != -1 {
		t.Fatalf("workchains: got %d,%d want 0,-1", got[0].Workchain(), got[1].Workchain())
	}
}

func TestLoadTargetsErrors(t *testing.T) {
	dir := t.TempDir()
	// Missing file.
	if _, err := loadTargets(filepath.Join(dir, "nope.txt")); err == nil {
		t.Fatal("expected error for missing file")
	}
	// Empty (only comments/blanks) => no addresses.
	p := filepath.Join(dir, "empty.txt")
	os.WriteFile(p, []byte("# just a comment\n\n"), 0o644)
	if _, err := loadTargets(p); err == nil {
		t.Fatal("expected error for file with no addresses")
	}
	// Malformed line.
	p2 := filepath.Join(dir, "bad.txt")
	os.WriteFile(p2, []byte("0:notvalidhex\n"), 0o644)
	if _, err := loadTargets(p2); err == nil {
		t.Fatal("expected error for malformed address")
	}
}

func TestNormalizeTransferModes(t *testing.T) {
	tests := map[string]string{
		"gram":            "gram",
		"ton":             "gram",
		"gassless-gram":   "gassless-gram",
		"gassless_gram":   "gassless-gram",
		"gasless-gram":    "gassless-gram",
		"gassless-jetton": "gassless-jetton",
		"gassless_jetton": "gassless-jetton",
		"gasless_jetton":  "gassless-jetton",
	}
	for input, want := range tests {
		if got := normalizeMode(input); got != want {
			t.Errorf("normalizeMode(%q)=%q want %q", input, got, want)
		}
	}
}
