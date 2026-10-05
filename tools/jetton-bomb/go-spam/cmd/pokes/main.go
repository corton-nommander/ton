// Throwaway: pull the 1-nanogram poke history off toncenter for every pool wallet [0,count], so we can
// see which big-state accounts got hit (and when) right before the shard split. Each pool wallet's
// getTransactions is fetched concurrently; out-messages with value==1 are the pokes. Sorted by time.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/xssnick/tonutils-go/address"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
	"github.com/victor/jetton-bomb-go-spam/pkg/walletspam"
)

type txResp struct {
	OK     bool `json:"ok"`
	Result []struct {
		Utime         int64 `json:"utime"`
		TransactionID struct {
			Hash string `json:"hash"`
			Lt   string `json:"lt"`
		} `json:"transaction_id"`
		OutMsgs []struct {
			Destination string `json:"destination"`
			Value       string `json:"value"`
		} `json:"out_msgs"`
	} `json:"result"`
}

type poke struct {
	utime  int64
	wallet int
	dst    string // friendly
	sizeB  int64
	hash   string // sender tx hash (base64)
}

func main() {
	endpoint := flag.String("e", "https://toncenter.com/api/v2", "toncenter v2 endpoint")
	key := flag.String("k", "", "api key")
	count := flag.Int("count", 5000, "pool ids [0,count]")
	minterStr := flag.String("minter", "EQCgKV2KK3U6AcRpGj64KzGjz8h6aCIkaV9Mce_rpubewSl_", "pool minter")
	sizesFile := flag.String("sizes", "./big_accounts.txt.sizes", "account\\tbytes file to annotate target sizes")
	limit := flag.Int("limit", 16, "getTransactions limit per wallet")
	workers := flag.Int("workers", 80, "concurrent fetchers")
	out := flag.String("out", "", "optional output file")
	flag.Parse()

	pub, _, err := backend.LoadKeypairFromSeedFile(backend.DefaultPoolKeyFile())
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	minter := address.MustParseAddr(*minterStr)
	code := walletspam.LoadCode(walletspam.CodeHex)
	jw := walletspam.LoadJettonWalletCode()

	// size lookup keyed by canonical raw (lowercase) address
	sizeByAddr := map[string]int64{}
	if data, err := os.ReadFile(*sizesFile); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			f := strings.Fields(line)
			if len(f) != 2 {
				continue
			}
			if a, err := address.ParseRawAddr(f[0]); err == nil {
				var n int64
				fmt.Sscanf(f[1], "%d", &n)
				sizeByAddr[a.StringRaw()] = n
			}
		}
	}
	fmt.Printf("loaded %d target sizes\n", len(sizeByAddr))

	client := &http.Client{Timeout: 30 * time.Second}
	getTx := func(ctx context.Context, addr string) (*txResp, error) {
		u := *endpoint + "/getTransactions?" + url.Values{
			"address": {addr}, "limit": {fmt.Sprint(*limit)}, "archival": {"true"},
		}.Encode()
		if *key != "" {
			u += "&api_key=" + *key
		}
		var last error
		for attempt := 0; attempt < 6; attempt++ {
			req, _ := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
			resp, err := client.Do(req)
			if err != nil {
				last = err
				time.Sleep(time.Duration(attempt+1) * 300 * time.Millisecond)
				continue
			}
			body, _ := io.ReadAll(resp.Body)
			resp.Body.Close()
			if resp.StatusCode == 429 || resp.StatusCode >= 500 {
				last = fmt.Errorf("status %d", resp.StatusCode)
				time.Sleep(time.Duration(attempt+1) * 400 * time.Millisecond)
				continue
			}
			var tr txResp
			if err := json.Unmarshal(body, &tr); err != nil {
				return nil, err
			}
			return &tr, nil
		}
		return nil, last
	}

	var (
		mu     sync.Mutex
		pokes  []poke
		done   int64
		failed int64
		wg     sync.WaitGroup
		ctx    = context.Background()
		idCh   = make(chan int, *workers*2)
	)
	for w := 0; w < *workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for id := range idCh {
				addr := walletspam.New(uint32(id), pub, nil, minter, jw, code).Address()
				tr, err := getTx(ctx, addr.String())
				if err != nil {
					atomic.AddInt64(&failed, 1)
					atomic.AddInt64(&done, 1)
					continue
				}
				var local []poke
				for _, t := range tr.Result {
					for _, m := range t.OutMsgs {
						if m.Value != "1" {
							continue // pokes are exactly 1 nanogram
						}
						sz := int64(0)
						if da, err := address.ParseAddr(m.Destination); err == nil {
							sz = sizeByAddr[da.StringRaw()]
						}
						local = append(local, poke{utime: t.Utime, wallet: id, dst: m.Destination, sizeB: sz, hash: t.TransactionID.Hash})
					}
				}
				if len(local) > 0 {
					mu.Lock()
					pokes = append(pokes, local...)
					mu.Unlock()
				}
				n := atomic.AddInt64(&done, 1)
				if n%500 == 0 {
					fmt.Fprintf(os.Stderr, "\r%d/%d wallets, %d pokes, %d failed", n, *count+1, len(pokes), atomic.LoadInt64(&failed))
				}
			}
		}()
	}
	for id := 0; id <= *count; id++ {
		idCh <- id
	}
	close(idCh)
	wg.Wait()
	fmt.Fprintf(os.Stderr, "\rscanned %d wallets, %d failed\n", atomic.LoadInt64(&done), atomic.LoadInt64(&failed))

	sort.Slice(pokes, func(i, j int) bool { return pokes[i].utime < pokes[j].utime }) // chronological

	var b strings.Builder
	b.WriteString("datetime_utc,tx_hash,sender_wallet,target,size_bytes\n")
	for _, p := range pokes {
		ts := time.Unix(p.utime, 0).UTC().Format("2006-01-02 15:04:05")
		b.WriteString(fmt.Sprintf("%s,%s,%d,%s,%d\n", ts, p.hash, p.wallet, p.dst, p.sizeB))
	}
	if *out != "" {
		_ = os.WriteFile(*out, []byte(b.String()), 0o644)
	}
	if len(pokes) > 0 {
		fmt.Printf("\n=== %d pokes (value=1), %s .. %s UTC ===\n", len(pokes),
			time.Unix(pokes[0].utime, 0).UTC().Format("15:04:05"),
			time.Unix(pokes[len(pokes)-1].utime, 0).UTC().Format("15:04:05"))
	}
	if *out != "" {
		fmt.Printf("wrote CSV: %s\n", *out)
	}
}
