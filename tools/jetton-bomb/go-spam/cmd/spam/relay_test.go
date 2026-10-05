package main

import (
	"testing"

	"github.com/xssnick/tonutils-go/tlb"
)

func TestRelayFundingModesAndOverrides(t *testing.T) {
	for _, tc := range []struct {
		name         string
		mode         string
		transfer     string
		general      string
		gramEnv      string
		gramAlias    string
		explicitFlag bool
		want         string
	}{
		{name: "highload GRAM", mode: "highload-gram", transfer: "0.001", want: "0.002"},
		{name: "pool-relayed GRAM", mode: "gassless-gram", transfer: "0.001", want: "0.002"},
		{name: "larger native transfer", mode: "highload-gram", transfer: "0.01", want: "0.011"},
		{name: "highload jetton", mode: "highload-jetton", want: "0.012"},
		{name: "pool-relayed jetton", mode: "gassless-jetton", want: "0.012"},
		{name: "general override", mode: "highload-gram", transfer: "0.001", general: "0.004", want: "0.004"},
		{name: "GRAM env wins", mode: "highload-gram", transfer: "0.001", general: "0.012", gramEnv: "0.003", want: "0.003"},
		{name: "GRAM alias", mode: "gassless-gram", transfer: "0.001", gramAlias: "0.003", want: "0.003"},
		{name: "primary env wins over alias", mode: "highload-gram", transfer: "0.001", gramEnv: "0.003", gramAlias: "0.004", want: "0.003"},
		{name: "flag wins over env", mode: "highload-gram", transfer: "0.001", general: "0.005", gramEnv: "0.003", explicitFlag: true, want: "0.005"},
		{name: "GRAM env does not affect jetton", mode: "highload-jetton", gramEnv: "0.002", want: "0.012"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Setenv("GASSLESS_GRAM_RELAY_VALUE", tc.gramEnv)
			t.Setenv("GASLESS_GRAM_RELAY_VALUE", tc.gramAlias)
			transfer := tlb.FromNanoTONU(0)
			if tc.transfer != "" {
				transfer = tlb.MustFromTON(tc.transfer)
			}
			got, err := resolveRelayValue(tc.mode, transfer, tc.general, tc.explicitFlag)
			if err != nil {
				t.Fatal(err)
			}
			if got.Nano().Cmp(tlb.MustFromTON(tc.want).Nano()) != 0 {
				t.Fatalf("relay value = %s, want %s", got.String(), tc.want)
			}
		})
	}
}

func TestRelayFundingRejectsInvalidOverrides(t *testing.T) {
	t.Setenv("GASSLESS_GRAM_RELAY_VALUE", "")
	t.Setenv("GASLESS_GRAM_RELAY_VALUE", "")
	for _, value := range []string{"0", "-0.001", "not-a-number"} {
		if _, err := resolveRelayValue("highload-gram", tlb.MustFromTON("0.001"), value, true); err == nil {
			t.Errorf("accepted invalid relay value %q", value)
		}
	}
}

func TestGaslessJettonDefaultFitsRelayFunding(t *testing.T) {
	t.Setenv("GASSLESS_JETTON_VALUE", "")
	for _, mode := range []string{"highload-jetton", "gassless-jetton"} {
		if got := jettonValueForMode(mode, "0.05", false); got != "0.01" {
			t.Fatalf("%s attaches %s, starving the default 0.012 relay", mode, got)
		}
		if got := jettonValueForMode(mode, "0.04", true); got != "0.04" {
			t.Fatal("explicit --value was lost")
		}
	}
	t.Setenv("GASSLESS_JETTON_VALUE", "0.02")
	if got := jettonValueForMode("highload-jetton", "0.05", false); got != "0.02" {
		t.Fatal("gasless env override was ignored")
	}
	if got := jettonValueForMode("jetton", "0.05", false); got != "0.05" {
		t.Fatal("changed ordinary jetton funding")
	}
}
