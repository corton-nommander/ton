package backend

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"math/big"
	"net/http"
	"strings"
	"time"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

// HTTPBackend talks to a Toncenter v2 REST endpoint.
// We use v2 because v3 has split sendMessage/runGetMethod across endpoints inconsistently.
type HTTPBackend struct {
	endpoint string // e.g. "https://toncenter.com/api/v2"
	apiKey   string
	client   *http.Client
}

// NewHTTP builds an HTTP backend. endpoint should point to the v2 root (without trailing slash, .../api/v2).
// Trailing /jsonRPC is stripped if present.
func NewHTTP(endpoint, apiKey string) *HTTPBackend {
	endpoint = strings.TrimSuffix(strings.TrimSuffix(endpoint, "/"), "/jsonRPC")
	return &HTTPBackend{
		endpoint: endpoint,
		apiKey:   apiKey,
		client:   &http.Client{Timeout: 30 * time.Second},
	}
}

func (b *HTTPBackend) Kind() Kind          { return KindHTTP }
func (b *HTTPBackend) Description() string { return "http " + b.endpoint }

type tcResponse struct {
	OK     bool            `json:"ok"`
	Result json.RawMessage `json:"result"`
	Error  string          `json:"error"`
	Code   int             `json:"code"`
}

func (b *HTTPBackend) post(ctx context.Context, path string, payload any) (json.RawMessage, error) {
	body, err := json.Marshal(payload)
	if err != nil {
		return nil, fmt.Errorf("marshal: %w", err)
	}
	url := b.endpoint + path
	req, err := http.NewRequestWithContext(ctx, "POST", url, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	if b.apiKey != "" {
		req.Header.Set("X-API-Key", b.apiKey)
	}
	resp, err := b.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	raw, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	var parsed tcResponse
	if err := json.Unmarshal(raw, &parsed); err != nil {
		return nil, fmt.Errorf("parse %s: %w; body=%s", path, err, truncateString(string(raw), 200))
	}
	if !parsed.OK {
		return nil, fmt.Errorf("toncenter %s: code=%d %s", path, parsed.Code, parsed.Error)
	}
	return parsed.Result, nil
}

func truncateString(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}

func (b *HTTPBackend) SendExternalMessage(ctx context.Context, dst *address.Address, body *cell.Cell) error {
	return b.sendBoc(ctx, buildExtMsgBoc(dst, body, nil))
}

func (b *HTTPBackend) SendExternalMessageWithInit(ctx context.Context, dst *address.Address, body *cell.Cell, stateInit *tlb.StateInit) error {
	var initCell *cell.Cell
	if stateInit != nil {
		c, err := tlb.ToCell(stateInit)
		if err != nil {
			return fmt.Errorf("serialize state init: %w", err)
		}
		initCell = c
	}
	return b.sendBoc(ctx, buildExtMsgBoc(dst, body, initCell))
}

func (b *HTTPBackend) sendBoc(ctx context.Context, ext []byte) error {
	payload := map[string]string{"boc": base64.StdEncoding.EncodeToString(ext)}
	_, err := b.post(ctx, "/sendBoc", payload)
	return err
}

// buildExtMsgBoc builds a TL-B external-in message BoC: ext_in_msg_info$10 src:MsgAddressExt
// dest:MsgAddressInt import_fee:Grams init:(Maybe (Either StateInit ^StateInit)) body:(Either X ^X).
// initCell (already-serialized state_init) is attached in a ref when non-nil, deploying the destination.
func buildExtMsgBoc(dst *address.Address, body, initCell *cell.Cell) []byte {
	c := cell.BeginCell().
		MustStoreUInt(0b10, 2). // ext_in_msg_info$10
		MustStoreUInt(0, 2).    // src: addr_none$00
		MustStoreAddr(dst).     // dest: MsgAddressInt
		MustStoreCoins(0)       // import_fee
	if initCell != nil {
		c = c.MustStoreUInt(1, 1). // init present (Maybe = 1)
						MustStoreUInt(1, 1). // Either: state_init stored in a ref
						MustStoreRef(initCell)
	} else {
		c = c.MustStoreUInt(0, 1) // no state init
	}
	// Body: always via ref (safe regardless of size).
	c = c.MustStoreUInt(1, 1).MustStoreRef(body)
	return c.EndCell().ToBOC()
}

func (b *HTTPBackend) GetSeqno(ctx context.Context, addr *address.Address) (uint32, error) {
	stack, err := b.RunGetMethod(ctx, addr, "seqno")
	if err != nil {
		return 0, err
	}
	if len(stack) == 0 {
		return 0, fmt.Errorf("seqno: empty stack")
	}
	n, err := stack[0].AsUint()
	if err != nil {
		return 0, fmt.Errorf("seqno: %w", err)
	}
	return uint32(n), nil
}

func (b *HTTPBackend) GetBalance(ctx context.Context, addr *address.Address) (*big.Int, error) {
	raw, err := b.post(ctx, "/getAddressInformation", map[string]string{"address": addr.String()})
	if err != nil {
		return nil, err
	}
	var info struct {
		Balance string `json:"balance"`
	}
	if err := json.Unmarshal(raw, &info); err != nil {
		return nil, fmt.Errorf("getAddressInformation: %w", err)
	}
	// Uninitialized accounts report "0" (and sometimes "-1" for nonexistent) — clamp to a non-negative int.
	n, ok := new(big.Int).SetString(info.Balance, 10)
	if !ok || n.Sign() < 0 {
		return big.NewInt(0), nil
	}
	return n, nil
}

// runGetMethodReq matches Toncenter v2 /runGetMethod input.
type runGetMethodReq struct {
	Address string  `json:"address"`
	Method  string  `json:"method"`
	Stack   [][]any `json:"stack"`
}

func (b *HTTPBackend) RunGetMethod(ctx context.Context, addr *address.Address, method string, args ...any) ([]StackValue, error) {
	stackArgs := make([][]any, 0, len(args))
	for _, a := range args {
		switch v := a.(type) {
		case int:
			stackArgs = append(stackArgs, []any{"num", fmt.Sprintf("%d", v)})
		case uint64:
			stackArgs = append(stackArgs, []any{"num", fmt.Sprintf("%d", v)})
		case *big.Int:
			stackArgs = append(stackArgs, []any{"num", v.String()})
		default:
			return nil, fmt.Errorf("unsupported arg type %T for runGetMethod", a)
		}
	}
	payload := runGetMethodReq{
		Address: addr.String(),
		Method:  method,
		Stack:   stackArgs,
	}
	raw, err := b.post(ctx, "/runGetMethod", payload)
	if err != nil {
		return nil, err
	}
	return parseRunResult(raw)
}

// parseRunResult parses { "@type":"smc.runResult", "stack":[[type, value], ...], "exit_code":..., ... }
func parseRunResult(raw json.RawMessage) ([]StackValue, error) {
	var r struct {
		Stack    [][]json.RawMessage `json:"stack"`
		ExitCode int                 `json:"exit_code"`
	}
	if err := json.Unmarshal(raw, &r); err != nil {
		return nil, fmt.Errorf("parse runResult: %w", err)
	}
	if r.ExitCode != 0 && r.ExitCode != 1 {
		return nil, fmt.Errorf("runGetMethod exit_code=%d", r.ExitCode)
	}
	out := make([]StackValue, 0, len(r.Stack))
	for i, entry := range r.Stack {
		if len(entry) != 2 {
			return nil, fmt.Errorf("stack[%d]: expected 2 elements", i)
		}
		var typ string
		if err := json.Unmarshal(entry[0], &typ); err != nil {
			return nil, fmt.Errorf("stack[%d] type: %w", i, err)
		}
		switch typ {
		case "num":
			var s string
			if err := json.Unmarshal(entry[1], &s); err != nil {
				return nil, fmt.Errorf("stack[%d] num: %w", i, err)
			}
			// Toncenter v2 returns ints as strings — sometimes signed-hex like "-0x1"
			// (TON boolean true = -1), sometimes decimal. big.Int.SetString(s, 0)
			// auto-detects base from a 0x/0o/0b/0 prefix and supports the leading sign.
			n := new(big.Int)
			if _, ok := n.SetString(s, 0); !ok {
				return nil, fmt.Errorf("stack[%d] num parse %q", i, s)
			}
			out = append(out, StackValue{Kind: "int", Int: n})
		case "cell":
			c, err := decodeBocPayload(entry[1])
			if err != nil {
				return nil, fmt.Errorf("stack[%d] cell: %w", i, err)
			}
			out = append(out, StackValue{Kind: "cell", Cell: c})
		case "slice":
			c, err := decodeBocPayload(entry[1])
			if err != nil {
				return nil, fmt.Errorf("stack[%d] slice: %w", i, err)
			}
			s := c.BeginParse()
			out = append(out, StackValue{Kind: "slice", Slice: s})
		default:
			out = append(out, StackValue{Kind: "unknown"})
		}
	}
	return out, nil
}

func decodeBocPayload(raw json.RawMessage) (*cell.Cell, error) {
	var v struct {
		Bytes string `json:"bytes"`
	}
	if err := json.Unmarshal(raw, &v); err != nil {
		return nil, err
	}
	bocBytes, err := base64.StdEncoding.DecodeString(v.Bytes)
	if err != nil {
		return nil, err
	}
	return cell.FromBOC(bocBytes)
}
