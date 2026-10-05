package tpsmeter

import (
	"bytes"
	"context"
	"encoding/base64"
	"fmt"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestToncenterShardsRejectsMissingOrEmptyPayload(t *testing.T) {
	tests := []string{"null", `{}`, `{"shards":null}`, `{"shards":[]}`, `{"shards":[{}]}`}
	for _, result := range tests {
		t.Run(result, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				fmt.Fprintf(w, `{"ok":true,"result":%s}`, result)
			}))
			defer server.Close()
			source := NewToncenterSource(ToncenterConfig{Base: server.URL})
			defer source.Close()
			if _, err := source.Shards(context.Background(), 10); err == nil {
				t.Fatalf("result %s unexpectedly produced shard tops", result)
			}
		})
	}
}

func TestToncenterShardsParsesNonEmptyPayload(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		fmt.Fprint(w, `{"ok":true,"result":{"shards":[{"workchain":0,"shard":"-9223372036854775808","seqno":7}]}}`)
	}))
	defer server.Close()
	source := NewToncenterSource(ToncenterConfig{Base: server.URL})
	defer source.Close()
	shards, err := source.Shards(context.Background(), 10)
	if err != nil {
		t.Fatal(err)
	}
	want := []BlockID{{Workchain: 0, Shard: testRootShard, Seqno: 7}}
	if len(shards) != 1 || shards[0] != want[0] {
		t.Fatalf("shards = %+v, want %+v", shards, want)
	}
}

func TestToncenterMasterchainIdentity(t *testing.T) {
	rootHash := bytes.Repeat([]byte{0x11}, 32)
	fileHash := bytes.Repeat([]byte{0x22}, 32)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		if request.URL.Path != "/lookupBlock" || request.URL.Query().Get("seqno") != "17" {
			t.Fatalf("unexpected request %s?%s", request.URL.Path, request.URL.RawQuery)
		}
		fmt.Fprintf(w, `{"ok":true,"result":{"root_hash":%q,"file_hash":%q}}`,
			base64.StdEncoding.EncodeToString(rootHash), base64.StdEncoding.EncodeToString(fileHash))
	}))
	defer server.Close()
	source := NewToncenterSource(ToncenterConfig{Base: server.URL})
	defer source.Close()
	identity, err := source.MasterchainIdentity(context.Background(), 17)
	if err != nil {
		t.Fatal(err)
	}
	want, err := blockIdentity(rootHash, fileHash)
	if err != nil {
		t.Fatal(err)
	}
	if identity != want {
		t.Fatalf("identity = %+v, want %+v", identity, want)
	}
}
