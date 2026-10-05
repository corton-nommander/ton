package sidechain

import (
	"testing"

	"github.com/xssnick/tonutils-go/tvm/cell"
)

func TestOrdinaryTVMExecutionBoundary(t *testing.T) {
	for _, test := range []struct {
		wc      int32
		version uint32
		want    bool
	}{
		{0, 13, true}, {0, 14, false}, {0, 15, false}, {0, 16, false},
		{-1, 13, true}, {-1, 14, true}, {-1, 16, true}, {1, 13, false},
	} {
		if got := OrdinaryTVMSupported(test.wc, test.version); got != test.want {
			t.Errorf("wc=%d version=%d: supported=%t want %t", test.wc, test.version, got, test.want)
		}
	}
}

func TestDecodeGlobalVersionPreservesCapabilities(t *testing.T) {
	const caps uint64 = 1<<63 | 3072 | 14
	param := cell.BeginCell().MustStoreUInt(0xc4, 8).MustStoreUInt(16, 32).MustStoreUInt(caps, 64).EndCell()
	version, got, err := DecodeGlobalVersion(param)
	if err != nil || version != 16 || got != caps {
		t.Fatalf("decoded version=%d capabilities=%#x err=%v", version, got, err)
	}
}

func TestDecodeGlobalVersionRejectsMalformedConfig(t *testing.T) {
	validPrefix := func() *cell.Builder {
		return cell.BeginCell().MustStoreUInt(0xc4, 8).MustStoreUInt(16, 32).MustStoreUInt(3072, 64)
	}
	for name, param := range map[string]*cell.Cell{
		"absent":          nil,
		"truncated":       cell.BeginCell().MustStoreUInt(0xc4, 8).MustStoreUInt(16, 32).EndCell(),
		"wrong tag":       cell.BeginCell().MustStoreUInt(0xc3, 8).MustStoreUInt(16, 32).MustStoreUInt(3072, 64).EndCell(),
		"extra bits":      validPrefix().MustStoreUInt(0, 1).EndCell(),
		"extra reference": validPrefix().MustStoreRef(cell.BeginCell().EndCell()).EndCell(),
	} {
		t.Run(name, func(t *testing.T) {
			if _, _, err := DecodeGlobalVersion(param); err == nil {
				t.Fatal("malformed config accepted")
			}
		})
	}
}
