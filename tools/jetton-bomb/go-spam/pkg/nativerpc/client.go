// Package nativerpc reads this fork's balance-only accounts and submits raw
// native envelopes. Stock tonutils-go's TVM account decoder cannot read them.
package nativerpc

import (
	"bytes"
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"math/big"
	"sync"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/sidechain"
	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/liteclient"
	"github.com/xssnick/tonutils-go/tl"
	"github.com/xssnick/tonutils-go/tlb"
	"github.com/xssnick/tonutils-go/ton"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

func init() {
	tl.Register(SendBatch{}, "liteServer.sendMessageBatch bodies:(vector bytes) = liteServer.SendMsgStatusBatch")
	tl.Register(SendResult{}, "liteServer.sendMsgResult status:int code:int message:string = liteServer.SendMsgResult")
	tl.Register(BatchResult{}, "liteServer.sendMsgStatusBatch results:(vector liteServer.sendMsgResult) = liteServer.SendMsgStatusBatch")
}

type SendBatch struct {
	Bodies [][]byte `tl:"vector bytes"`
}
type SendResult struct {
	Status  int32  `tl:"int" json:"status"`
	Code    int32  `tl:"int" json:"code"`
	Message string `tl:"string" json:"message"`
}
type BatchResult struct {
	Results []SendResult `tl:"vector struct"`
}

// Native custom block data is deliberately left raw: it is not McBlockExtra.
type envelope struct {
	_           tlb.Magic  `tlb:"#11ef55aa"`
	GlobalID    int32      `tlb:"## 32"`
	Info        *cell.Cell `tlb:"^"`
	ValueFlow   *cell.Cell `tlb:"^"`
	StateUpdate *cell.Cell `tlb:"^"`
	Extra       *cell.Cell `tlb:"^"`
}

type Account struct {
	Address   string                  `json:"address"`
	Balance   uint64                  `json:"balance_nanograms"`
	Nonce     uint64                  `json:"nonce"`
	Flags     uint8                   `json:"flags"`
	StateHash string                  `json:"state_hash"`
	Shard     sidechain.BlockIdentity `json:"shard"`
}
type Snapshot struct {
	CapturedAt  time.Time                 `json:"captured_at"`
	Masterchain sidechain.BlockIdentity   `json:"masterchain"`
	ChainTime   uint32                    `json:"chain_time"`
	Shards      []sidechain.BlockIdentity `json:"shards"`
	Accounts    []Account                 `json:"accounts"`
}
type Client struct {
	Pool *liteclient.ConnectionPool
	API  *ton.APIClient
}

func Connect(ctx context.Context, config string) (*Client, error) {
	cfg, err := liteclient.GetConfigFromFile(config)
	if err != nil {
		return nil, err
	}
	p := liteclient.NewConnectionPool()
	if err = p.AddConnectionsFromConfig(ctx, cfg); err != nil {
		p.Stop()
		return nil, err
	}
	return &Client{p, ton.NewAPIClient(p, ton.ProofCheckPolicyFast)}, nil
}
func (c *Client) Close() { c.Pool.Stop() }
func Identity(id *ton.BlockIDExt) sidechain.BlockIdentity {
	return sidechain.BlockIdentity{Workchain: id.Workchain, Shard: fmt.Sprintf("%016X", uint64(id.Shard)), Seqno: id.SeqNo,
		RootHash: hex.EncodeToString(id.RootHash), FileHash: hex.EncodeToString(id.FileHash)}
}
func (c *Client) Latest(ctx context.Context) (*ton.BlockIDExt, error) {
	var response tl.Serializable
	if err := c.Pool.QueryLiteserver(ctx, ton.GetMasterchainInf{}, &response); err != nil {
		return nil, err
	}
	if result, ok := response.(ton.MasterchainInfo); ok && result.Last != nil {
		return result.Last, nil
	}
	if err, ok := response.(ton.LSError); ok {
		return nil, err
	}
	return nil, fmt.Errorf("unexpected masterchain response %T", response)
}
func (c *Client) Send(ctx context.Context, bodies [][]byte) ([]SendResult, error) {
	if len(bodies) < 1 || len(bodies) > 1024 {
		return nil, errors.New("batch requires 1..1024 envelopes")
	}
	bytes := 0
	for _, body := range bodies {
		if len(body) == 0 || len(body) > (8<<20)-bytes {
			return nil, errors.New("batch bodies must be nonempty and total at most 8 MiB")
		}
		bytes += len(body)
	}
	var response tl.Serializable
	if err := c.Pool.QueryLiteserver(ctx, SendBatch{bodies}, &response); err != nil {
		return nil, err
	}
	if result, ok := response.(BatchResult); ok {
		if len(result.Results) != len(bodies) {
			return nil, errors.New("batch result length mismatch")
		}
		for _, r := range result.Results {
			if r.Status != 0 && r.Status != 1 || r.Status == 1 && (r.Code != 0 || r.Message != "") {
				return nil, errors.New("invalid batch status")
			}
		}
		return result.Results, nil
	}
	if err, ok := response.(ton.LSError); ok {
		return nil, err
	}
	return nil, fmt.Errorf("unexpected batch response %T", response)
}

func DecodeAccount(root *cell.Cell) (uint64, uint64, uint8, error) {
	if root == nil {
		return 0, 0, 0, errors.New("native account is absent")
	}
	s := root.BeginParse()
	if s.BitsLeft() != 138 || s.RefsNum() != 0 {
		return 0, 0, 0, errors.New("native account must be exactly 138 bits without refs")
	}
	tag, err := s.LoadUInt(2)
	if err != nil || tag != 1 {
		return 0, 0, 0, errors.New("account is not account_native$01")
	}
	b, _ := s.LoadUInt(64)
	n, _ := s.LoadUInt(64)
	f, _ := s.LoadUInt(8)
	return b, n, uint8(f), nil
}

func (c *Client) Account(ctx context.Context, master, expectedShard *ton.BlockIDExt, addr *address.Address) (Account, error) {
	if addr == nil || addr.Workchain() != 0 || len(addr.Data()) != 32 || master == nil || expectedShard == nil {
		return Account{}, errors.New("native account reads require a basechain address and explicit masterchain/shard anchors")
	}
	var response tl.Serializable
	if err := c.Pool.QueryLiteserver(ctx, ton.GetAccountState{ID: master, Account: ton.AccountID{Workchain: 0, ID: addr.Data()}}, &response); err != nil {
		return Account{}, err
	}
	r, ok := response.(ton.AccountState)
	if !ok {
		if err, ok := response.(ton.LSError); ok {
			return Account{}, err
		}
		return Account{}, fmt.Errorf("unexpected account response %T", response)
	}
	if r.ID == nil || !r.ID.Equals(master) || r.Shard == nil || !r.Shard.Equals(expectedShard) {
		return Account{}, errors.New("account response differs from exact masterchain/shard anchor")
	}
	if len(r.Proof) != 2 || r.State == nil {
		return Account{}, errors.New("native account or state proofs missing")
	}
	// The topology was separately Merkle checked against master.RootHash.
	blockRoot, err := cell.UnwrapProof(r.Proof[0], expectedShard.RootHash)
	if err != nil {
		return Account{}, fmt.Errorf("account block proof: %w", err)
	}
	var block envelope
	if err = tlb.LoadFromCellAsProof(&block, blockRoot.BeginParse(), false); err != nil {
		return Account{}, err
	}
	var header tlb.BlockHeader
	if err = tlb.LoadFromCellAsProof(&header, block.Info.BeginParse(), false); err != nil {
		return Account{}, err
	}
	if !matchesShard(header.Shard, header.SeqNo, expectedShard) || !header.NotMaster {
		return Account{}, errors.New("account block header identity differs from shard anchor")
	}
	if block.StateUpdate.GetType() != cell.MerkleUpdateCellType || block.StateUpdate.RefsNum() != 2 {
		return Account{}, errors.New("account block requires a canonical Merkle state update")
	}
	newState, err := block.StateUpdate.PeekRef(1)
	if err != nil {
		return Account{}, err
	}
	stateRoot, err := cell.UnwrapProof(r.Proof[1], newState.Hash(0))
	if err != nil {
		return Account{}, fmt.Errorf("account shard state proof: %w", err)
	}
	var state tlb.ShardStateUnsplit
	if err = tlb.LoadFromCellAsProof(&state, stateRoot.BeginParse(), false); err != nil {
		return Account{}, err
	}
	if !matchesShard(state.ShardIdent, state.Seqno, expectedShard) {
		return Account{}, errors.New("account shard state identity differs from shard anchor")
	}
	if state.Accounts.ShardAccounts == nil {
		return Account{}, errors.New("account dictionary absent")
	}
	v := state.Accounts.ShardAccounts.Get(cell.BeginCell().MustStoreSlice(addr.Data(), 256).EndCell())
	if v == nil {
		return Account{}, errors.New("funded native account absent from proof")
	}
	s := v.BeginParse()
	var balanceInfo tlb.DepthBalanceInfo
	var shardAccount tlb.ShardAccount
	if err = tlb.LoadFromCell(&balanceInfo, s); err != nil {
		return Account{}, err
	}
	if err = tlb.LoadFromCell(&shardAccount, s); err != nil {
		return Account{}, err
	}
	if !bytes.Equal(shardAccount.Account.Hash(0), r.State.Hash(0)) {
		return Account{}, errors.New("account state differs from Merkle proof")
	}
	balance, nonce, flags, err := DecodeAccount(r.State)
	if err != nil {
		return Account{}, err
	}
	if balanceInfo.Currencies.Coins.Nano().Cmp(new(big.Int).SetUint64(balance)) != 0 {
		return Account{}, errors.New("account balance differs from augmented dictionary")
	}
	if balanceInfo.Currencies.ExtraCurrencies != nil && balanceInfo.Currencies.ExtraCurrencies.Size() != 0 {
		return Account{}, errors.New("native account extra currencies unsupported")
	}
	return Account{addr.StringRaw(), balance, nonce, flags, hex.EncodeToString(r.State.Hash()), Identity(expectedShard)}, nil
}

func ShardContains(shard int64, addr *address.Address) bool {
	x := new(big.Int).SetBytes(addr.Data()[:8]).Uint64()
	s := uint64(shard)
	low := s & -s
	return low != 0 && (x & ^(low*2-1)) == (s & ^(low*2-1))
}

func matchesShard(id tlb.ShardIdent, seq uint32, expected *ton.BlockIDExt) bool {
	if id.PrefixBits < 0 || id.PrefixBits > 60 {
		return false
	}
	shard := id.ShardPrefix | uint64(1)<<(63-uint(id.PrefixBits))
	return id.WorkchainID == expected.Workchain && seq == expected.SeqNo && shard == uint64(expected.Shard)
}

// Snapshot validates account inclusion and shard roots against one observed
// masterchain root. The local liteserver supplies that trusted root; consensus
// signatures/proof-chain finality are not independently verified here.
func (c *Client) Snapshot(ctx context.Context, addresses []*address.Address, workers int) (*Snapshot, error) {
	if workers < 1 || workers > 1024 || len(addresses) == 0 {
		return nil, errors.New("snapshot needs nonempty accounts and 1..1024 workers")
	}
	for _, addr := range addresses {
		if addr == nil || addr.Workchain() != 0 || len(addr.Data()) != 32 {
			return nil, errors.New("snapshot requires native basechain addresses")
		}
	}
	mc, err := c.Latest(ctx)
	if err != nil {
		return nil, err
	}
	captured := time.Now().UTC()
	shards, err := c.API.GetBlockShardsInfo(ctx, mc)
	if err != nil {
		return nil, err
	}
	header, err := c.API.GetBlockHeader(ctx, mc)
	if err != nil {
		return nil, err
	}
	result := &Snapshot{CapturedAt: captured, Masterchain: Identity(mc), ChainTime: header.GenUtime, Accounts: make([]Account, len(addresses))}
	for _, s := range shards {
		result.Shards = append(result.Shards, Identity(s))
	}
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	jobs := make(chan int)
	var wg sync.WaitGroup
	var firstErr error
	var once sync.Once
	for range workers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range jobs {
				var shard *ton.BlockIDExt
				for _, s := range shards {
					if s.Workchain == 0 && ShardContains(s.Shard, addresses[i]) {
						shard = s
						break
					}
				}
				var e error
				if shard == nil {
					e = errors.New("source outside anchored basechain topology")
				} else {
					result.Accounts[i], e = c.Account(ctx, mc, shard, addresses[i])
				}
				if e != nil {
					once.Do(func() { firstErr = fmt.Errorf("%s: %w", addresses[i].StringRaw(), e); cancel() })
				}
			}
		}()
	}
submit:
	for i := range addresses {
		select {
		case jobs <- i:
		case <-ctx.Done():
			break submit
		}
	}
	close(jobs)
	wg.Wait()
	if firstErr != nil {
		return nil, firstErr
	}
	if ctx.Err() != nil {
		return nil, ctx.Err()
	}
	return result, nil
}
