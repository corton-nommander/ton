package main

import "testing"

func TestDefaultLSConfigPath(t *testing.T) {
	t.Setenv("HOME", "/home/alice")
	if got, want := defaultLSConfigPath(), "/home/alice/genesis/devnet-liteservers-only.json"; got != want {
		t.Fatalf("defaultLSConfigPath() = %q, want %q", got, want)
	}
}
