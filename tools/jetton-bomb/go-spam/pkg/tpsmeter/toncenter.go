package tpsmeter

import (
	"context"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Toncenter v2 pagination / retry tuning (mirrors tpsmeter.py).
const (
	tcTxPageSize  = 1024
	tcTxPageLimit = 128 // 128 * 1024 = 131k tx cap per block before recording partial
	tcRetries     = 4
)

// ToncenterSource walks blocks over a Toncenter v2 HTTP API. Endpoints are
// appended to the base as-is (e.g. base "http://devnet-01.toncenter.com" ->
// ".../getMasterchainInfo", or "https://toncenter.com/api/v2" for mainnet).
type ToncenterSource struct {
	base    string
	apiKey  string
	client  *http.Client
	bucket  *tokenBucket
	retries int
}

// ToncenterConfig configures the HTTP source.
type ToncenterConfig struct {
	Base    string        // API base URL (path is appended directly)
	APIKey  string        // optional X-API-Key
	Timeout time.Duration // per-request timeout (default 20s)
	RPS     float64       // outbound rate cap on ALL requests (0 = unlimited); protects the API during backfill
	MaxConn int           // http transport connection pool (default 32)
}

// NewToncenterSource builds a Toncenter v2 source.
func NewToncenterSource(cfg ToncenterConfig) *ToncenterSource {
	if cfg.Timeout <= 0 {
		cfg.Timeout = 20 * time.Second
	}
	if cfg.MaxConn <= 0 {
		cfg.MaxConn = 32
	}
	tr := &http.Transport{
		MaxConnsPerHost:     cfg.MaxConn,
		MaxIdleConnsPerHost: cfg.MaxConn,
		IdleConnTimeout:     30 * time.Second,
	}
	return &ToncenterSource{
		base:    strings.TrimRight(cfg.Base, "/"),
		apiKey:  cfg.APIKey,
		client:  &http.Client{Timeout: cfg.Timeout, Transport: tr},
		bucket:  newTokenBucket(cfg.RPS), // applied to every request (0 = unlimited)
		retries: tcRetries,
	}
}

func (s *ToncenterSource) Description() string { return "toncenter " + s.base }

// Close releases idle HTTP connections.
func (s *ToncenterSource) Close() { s.client.CloseIdleConnections() }

// getResult performs GET base/path?params and unwraps the {ok,result,error} envelope.
func (s *ToncenterSource) getResult(ctx context.Context, path string, params url.Values) (json.RawMessage, error) {
	var last error
	for attempt := 0; attempt <= s.retries; attempt++ {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		s.bucket.take()
		u := s.base + "/" + path
		if len(params) > 0 {
			u += "?" + params.Encode()
		}
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
		if err != nil {
			return nil, err
		}
		if s.apiKey != "" {
			req.Header.Set("X-API-Key", s.apiKey)
		}
		resp, err := s.client.Do(req)
		if err != nil {
			last = err
			time.Sleep(backoff(attempt))
			continue
		}
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if resp.StatusCode == http.StatusTooManyRequests {
			ra := parseRetryAfter(resp.Header.Get("Retry-After"))
			min := time.Duration(250*(attempt+1)) * time.Millisecond
			if ra > min {
				min = ra
			}
			time.Sleep(min)
			last = fmt.Errorf("http 429 on %s", path)
			continue
		}
		if resp.StatusCode/100 != 2 {
			last = fmt.Errorf("http %d on %s: %s", resp.StatusCode, path, truncate(body, 160))
			time.Sleep(backoff(attempt))
			continue
		}
		var env struct {
			OK     bool            `json:"ok"`
			Result json.RawMessage `json:"result"`
			Error  string          `json:"error"`
		}
		if err := json.Unmarshal(body, &env); err != nil {
			last = fmt.Errorf("parse %s: %w; body=%s", path, err, truncate(body, 160))
			continue
		}
		if !env.OK {
			return nil, fmt.Errorf("api %s error: %s", path, env.Error)
		}
		return env.Result, nil
	}
	return nil, last
}

func (s *ToncenterSource) LatestMasterchainSeqno(ctx context.Context) (uint32, error) {
	raw, err := s.getResult(ctx, "getMasterchainInfo", nil)
	if err != nil {
		return 0, err
	}
	var r struct {
		Last struct {
			Seqno uint32 `json:"seqno"`
		} `json:"last"`
	}
	if err := json.Unmarshal(raw, &r); err != nil {
		return 0, fmt.Errorf("getMasterchainInfo: %w", err)
	}
	return r.Last.Seqno, nil
}

func (s *ToncenterSource) MasterchainIdentity(ctx context.Context, seqno uint32) (BlockIdentity, error) {
	raw, err := s.getResult(ctx, "lookupBlock", url.Values{
		"workchain": {strconv.FormatInt(int64(mcWorkchain), 10)},
		"shard":     {strconv.FormatInt(mcShard, 10)},
		"seqno":     {strconv.FormatUint(uint64(seqno), 10)},
	})
	if err != nil {
		return BlockIdentity{}, err
	}
	var result struct {
		RootHash string `json:"root_hash"`
		FileHash string `json:"file_hash"`
		ID       *struct {
			RootHash string `json:"root_hash"`
			FileHash string `json:"file_hash"`
		} `json:"id"`
	}
	if string(bytesTrimSpace(raw)) == "null" {
		return BlockIdentity{}, fmt.Errorf("lookupBlock MC %d returned null", seqno)
	}
	if err := json.Unmarshal(raw, &result); err != nil {
		return BlockIdentity{}, fmt.Errorf("lookupBlock MC %d: %w", seqno, err)
	}
	if result.ID != nil && result.RootHash == "" && result.FileHash == "" {
		result.RootHash, result.FileHash = result.ID.RootHash, result.ID.FileHash
	}
	rootHash, err := decodeHash32(result.RootHash)
	if err != nil {
		return BlockIdentity{}, fmt.Errorf("lookupBlock MC %d root_hash: %w", seqno, err)
	}
	fileHash, err := decodeHash32(result.FileHash)
	if err != nil {
		return BlockIdentity{}, fmt.Errorf("lookupBlock MC %d file_hash: %w", seqno, err)
	}
	return blockIdentity(rootHash, fileHash)
}

func (s *ToncenterSource) Shards(ctx context.Context, mcSeqno uint32) ([]BlockID, error) {
	raw, err := s.getResult(ctx, "shards", url.Values{"seqno": {strconv.FormatUint(uint64(mcSeqno), 10)}})
	if err != nil {
		return nil, err
	}
	if string(bytesTrimSpace(raw)) == "null" {
		return nil, fmt.Errorf("shards MC %d returned null", mcSeqno)
	}
	var envelope struct {
		Shards json.RawMessage `json:"shards"`
	}
	if err := json.Unmarshal(raw, &envelope); err != nil {
		return nil, fmt.Errorf("shards: %w", err)
	}
	if len(envelope.Shards) == 0 || string(bytesTrimSpace(envelope.Shards)) == "null" {
		return nil, fmt.Errorf("shards MC %d payload is missing or null", mcSeqno)
	}
	var shards []struct {
		Workchain *int32     `json:"workchain"`
		Shard     *flexInt64 `json:"shard"`
		Seqno     *uint32    `json:"seqno"`
	}
	if err := json.Unmarshal(envelope.Shards, &shards); err != nil {
		return nil, fmt.Errorf("shards: %w", err)
	}
	if len(shards) == 0 {
		return nil, fmt.Errorf("shards MC %d returned no shard tops", mcSeqno)
	}
	out := make([]BlockID, 0, len(shards))
	for i, sh := range shards {
		if sh.Workchain == nil || sh.Shard == nil || sh.Seqno == nil {
			return nil, fmt.Errorf("shards MC %d entry %d is missing block coordinates", mcSeqno, i)
		}
		out = append(out, BlockID{Workchain: *sh.Workchain, Shard: int64(*sh.Shard), Seqno: *sh.Seqno})
	}
	return out, nil
}

func (s *ToncenterSource) BlockUtime(ctx context.Context, id BlockID) (int64, error) {
	shardStr := strconv.FormatInt(id.Shard, 10)
	seqnoStr := strconv.FormatUint(uint64(id.Seqno), 10)
	wcStr := strconv.FormatInt(int64(id.Workchain), 10)

	hraw, err := s.getResult(ctx, "getBlockHeader", url.Values{
		"workchain": {wcStr}, "shard": {shardStr}, "seqno": {seqnoStr},
	})
	if err != nil {
		return 0, err
	}
	var h struct {
		GenUtime int64 `json:"gen_utime"`
	}
	if err := json.Unmarshal(hraw, &h); err != nil {
		return 0, fmt.Errorf("getBlockHeader: %w", err)
	}
	return h.GenUtime, nil
}

func (s *ToncenterSource) BlockInfo(ctx context.Context, id BlockID) (BlockInfo, error) {
	genUtime, err := s.BlockUtime(ctx, id)
	if err != nil {
		return BlockInfo{}, err
	}
	shardStr := strconv.FormatInt(id.Shard, 10)
	seqnoStr := strconv.FormatUint(uint64(id.Seqno), 10)
	wcStr := strconv.FormatInt(int64(id.Workchain), 10)

	total, partial := 0, false
	params := url.Values{
		"workchain": {wcStr}, "shard": {shardStr}, "seqno": {seqnoStr},
		"count": {strconv.Itoa(tcTxPageSize)},
	}
	for page := 0; page < tcTxPageLimit; page++ {
		traw, err := s.getResult(ctx, "getBlockTransactions", params)
		if err != nil {
			return BlockInfo{}, err
		}
		var t struct {
			Transactions []struct {
				LT   json.RawMessage `json:"lt"`
				Hash string          `json:"hash"`
			} `json:"transactions"`
			Incomplete bool `json:"incomplete"`
		}
		if err := json.Unmarshal(traw, &t); err != nil {
			return BlockInfo{}, fmt.Errorf("getBlockTransactions: %w", err)
		}
		total += len(t.Transactions)
		if !t.Incomplete {
			partial = false
			break
		}
		if len(t.Transactions) == 0 { // API said incomplete but returned nothing
			partial = true
			break
		}
		if page == tcTxPageLimit-1 {
			partial = true
			break
		}
		last := t.Transactions[len(t.Transactions)-1]
		params.Set("after_lt", strings.Trim(string(last.LT), `"`))
		params.Set("after_hash", last.Hash)
	}
	return BlockInfo{GenUtime: genUtime, TxCount: total, Partial: partial}, nil
}

// flexInt64 unmarshals a JSON int whether it arrives quoted ("−9223...") or bare.
type flexInt64 int64

func (f *flexInt64) UnmarshalJSON(b []byte) error {
	s := strings.Trim(string(b), `"`)
	if s == "" || s == "null" {
		*f = 0
		return nil
	}
	v, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return err
	}
	*f = flexInt64(v)
	return nil
}

func backoff(attempt int) time.Duration {
	return time.Duration(100*(attempt+1)) * time.Millisecond
}

func parseRetryAfter(h string) time.Duration {
	if h == "" {
		return 0
	}
	if secs, err := strconv.ParseFloat(strings.TrimSpace(h), 64); err == nil && secs > 0 {
		return time.Duration(secs * float64(time.Second))
	}
	return 0
}

func truncate(b []byte, n int) string {
	if len(b) <= n {
		return string(b)
	}
	return string(b[:n]) + "..."
}

func decodeHash32(value string) ([]byte, error) {
	if value == "" {
		return nil, fmt.Errorf("hash is missing")
	}
	for _, encoding := range []*base64.Encoding{base64.StdEncoding, base64.RawStdEncoding, base64.URLEncoding, base64.RawURLEncoding} {
		decoded, err := encoding.DecodeString(value)
		if err == nil && len(decoded) == 32 {
			return decoded, nil
		}
	}
	decoded, err := hex.DecodeString(value)
	if err == nil && len(decoded) == 32 {
		return decoded, nil
	}
	return nil, fmt.Errorf("expected a 32-byte base64 or hex hash")
}

func bytesTrimSpace(raw []byte) []byte {
	return []byte(strings.TrimSpace(string(raw)))
}

// tokenBucket is a simple thread-safe RPS limiter with a small burst.
type tokenBucket struct {
	mu     sync.Mutex
	rps    float64
	cap    float64
	tokens float64
	last   time.Time
}

func newTokenBucket(rps float64) *tokenBucket {
	if rps <= 0 {
		return nil
	}
	c := math.Max(rps, 8)
	return &tokenBucket{rps: rps, cap: c, tokens: c, last: time.Now()}
}

func (t *tokenBucket) take() {
	if t == nil {
		return
	}
	for {
		t.mu.Lock()
		now := time.Now()
		t.tokens = math.Min(t.cap, t.tokens+now.Sub(t.last).Seconds()*t.rps)
		t.last = now
		if t.tokens >= 1 {
			t.tokens -= 1
			t.mu.Unlock()
			return
		}
		wait := time.Duration((1 - t.tokens) / t.rps * float64(time.Second))
		t.mu.Unlock()
		time.Sleep(wait)
	}
}
