// sidechaincheck reads protocol configuration before an ordinary TVM benchmark.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/sidechain"
)

func main() {
	config := flag.String("ls-config", os.Getenv("LS_CONFIG"), "local TON global config JSON path")
	workchain := flag.Int("workchain", 0, "workchain for ordinary TVM load: 0 or -1")
	timeout := flag.Duration("timeout", 30*time.Second, "total read-only preflight timeout")
	flag.Parse()
	if *config == "" || *timeout <= 0 || (*workchain != 0 && *workchain != -1) {
		fmt.Fprintln(os.Stderr, "--ls-config is required, --timeout must be positive, and --workchain must be 0 or -1")
		os.Exit(2)
	}
	ctx, cancel := context.WithTimeout(context.Background(), *timeout)
	defer cancel()
	report, err := sidechain.RequireTVM(ctx, *config, int32(*workchain))
	if report != nil {
		encoder := json.NewEncoder(os.Stdout)
		encoder.SetIndent("", "  ")
		if encodeErr := encoder.Encode(report); encodeErr != nil {
			fmt.Fprintln(os.Stderr, encodeErr)
			os.Exit(1)
		}
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		if errors.Is(err, sidechain.ErrUnsupportedTVM) {
			os.Exit(sidechain.UnsupportedExitCode)
		}
		os.Exit(1)
	}
}
