// Throwaway: print the raw wc:hex addresses of pool wallets [0,count] for the mainnet pool, so we can
// feed them to the toncenter archival PG (tonaddr wants raw form) and pull the poke history.
package main

import (
	"bufio"
	"fmt"
	"os"

	"github.com/xssnick/tonutils-go/address"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

func main() {
	count := 5000
	minterStr := "EQCgKV2KK3U6AcRpGj64KzGjz8h6aCIkaV9Mce_rpubewSl_"
	if len(os.Args) > 1 {
		fmt.Sscanf(os.Args[1], "%d", &count)
	}
	if len(os.Args) > 2 {
		minterStr = os.Args[2]
	}
	pub, _, err := backend.LoadKeypairFromSeedFile(backend.DefaultPoolKeyFile())
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	minter := address.MustParseAddr(minterStr)
	code := walletspam.LoadCode(walletspam.CodeHex)
	jw := walletspam.LoadJettonWalletCode()

	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	for id := 0; id <= count; id++ {
		a := walletspam.New(uint32(id), pub, nil, minter, jw, code).Address()
		fmt.Fprintln(w, a.StringRaw())
	}
}
