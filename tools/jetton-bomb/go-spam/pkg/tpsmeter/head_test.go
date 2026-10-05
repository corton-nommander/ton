package tpsmeter

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

type headerOnlySource struct {
	nopSource
	shards []BlockID
	times  map[BlockID]int64
	fails  BlockID
}

func (*headerOnlySource) LatestMasterchainSeqno(context.Context) (uint32, error) { return 42, nil }
func (s *headerOnlySource) Shards(_ context.Context, mc uint32) ([]BlockID, error) {
	if mc != 42 {
		return nil, errors.New("shards queried for a different masterchain block")
	}
	return s.shards, nil
}
func (s *headerOnlySource) BlockUtime(_ context.Context, id BlockID) (int64, error) {
	if id == s.fails {
		return 0, errors.New("missing header")
	}
	return s.times[id], nil
}
func (*headerOnlySource) BlockInfo(context.Context, BlockID) (BlockInfo, error) {
	panic("head endpoint must not scan transactions")
}

func TestHTTPHeadUsesHeadersFromOneMasterchainBlock(t *testing.T) {
	mc := BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 42}
	left := BlockID{Workchain: 0, Shard: 0x4000000000000000, Seqno: 110}
	right := BlockID{Workchain: 0, Shard: -4611686018427387904, Seqno: 101}
	source := &headerOnlySource{
		shards: []BlockID{right, {Workchain: mcWorkchain, Shard: mcShard, Seqno: 42}, left},
		times:  map[BlockID]int64{mc: 1_700_000_042, left: 1_700_000_040, right: 1_700_000_038},
	}
	handler := NewHTTPHandler(New(source, Options{Workers: 2}), nil)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/head", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("head status = %d: %s", response.Code, response.Body.String())
	}
	var head LiveHead
	if err := json.Unmarshal(response.Body.Bytes(), &head); err != nil {
		t.Fatal(err)
	}
	if head.MC.Seqno != 42 || head.MC.GenUtime != 1_700_000_042 || head.MC.Workchain != -1 ||
		len(head.Shards) != 2 || head.Shards[0].Shard != "4000000000000000" || head.Shards[0].GenUtime != 1_700_000_040 ||
		head.Shards[1].Shard != "C000000000000000" || head.Shards[1].Seqno != 101 || head.Shards[1].GenUtime != 1_700_000_038 {
		t.Fatalf("head is not a coherent ordered MC/shard snapshot: %+v", head)
	}
	if !strings.Contains(response.Body.String(), `"gen_utime":"1700000042"`) {
		t.Fatalf("block time must retain the cockpit's quoted JSON format: %s", response.Body.String())
	}
}

func TestHTTPHeadRefusesPartialShardSnapshot(t *testing.T) {
	mc := BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: 42}
	shard := BlockID{Workchain: 0, Shard: 0x4000000000000000, Seqno: 110}
	source := &headerOnlySource{shards: []BlockID{shard}, times: map[BlockID]int64{mc: 1_700_000_042}, fails: shard}
	handler := NewHTTPHandler(New(source, Options{}), nil)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/v1/head", nil))
	if response.Code != http.StatusBadGateway || strings.Contains(response.Body.String(), `"shards"`) {
		t.Fatalf("partial head was served: %d %s", response.Code, response.Body.String())
	}
}
