// derivekey converts a 24-word TON mnemonic into the raw ed25519 secret (hex), so you can move the
// control wallet (or any wallet) off mnemonics onto WALLET_SECRET / a seed file without changing its
// address. Prints the 32-byte seed, the 64-byte private key, and the public key — all hex.
//
// Usage (mnemonic from env, file, or args):
//
//	WALLET_MNEMONIC="word1 word2 ... word24" go run ./cmd/derivekey
//	go run ./cmd/derivekey ./mnemonic.txt
//	go run ./cmd/derivekey word1 word2 ... word24
package main

import (
	"crypto/ed25519"
	"encoding/hex"
	"fmt"
	"os"
	"strings"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
)

func main() {
	mnemonic := os.Getenv("WALLET_MNEMONIC")
	if args := os.Args[1:]; len(args) > 0 {
		// One arg that points at an existing file → read it; otherwise treat args as the words.
		if len(args) == 1 {
			if b, err := os.ReadFile(args[0]); err == nil {
				mnemonic = string(b)
			} else {
				mnemonic = args[0]
			}
		} else {
			mnemonic = strings.Join(args, " ")
		}
	}
	if strings.TrimSpace(mnemonic) == "" {
		fmt.Fprintln(os.Stderr, "provide a mnemonic via WALLET_MNEMONIC, a file path, or as args")
		os.Exit(1)
	}

	_, priv, err := backend.LoadKeypairFromMnemonic(mnemonic)
	if err != nil {
		fmt.Fprintf(os.Stderr, "%v\n", err)
		os.Exit(1)
	}
	pub := priv.Public().(ed25519.PublicKey)

	fmt.Printf("SEED_HEX=%s\n", hex.EncodeToString(priv.Seed())) // 32 bytes → WALLET_SECRET or pool.key
	fmt.Printf("SECRET_HEX=%s\n", hex.EncodeToString(priv))      // 64 bytes → WALLET_SECRET (either works)
	fmt.Printf("PUBKEY_HEX=%s\n", hex.EncodeToString(pub))
}
