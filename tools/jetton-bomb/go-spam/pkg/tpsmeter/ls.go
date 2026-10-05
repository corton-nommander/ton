package tpsmeter

import (
	"context"
	"fmt"
	"sync"

	"github.com/xssnick/tonutils-go/liteclient"
	"github.com/xssnick/tonutils-go/ton"
)

// lsCacheCap bounds the per-poll BlockIDExt cache before it is reset.
const (
	lsCacheCap   = 8192
	lsTxPageSize = 256
	lsTxPages    = 512 // 256 * 512 = 131072 tx cap per block before recording partial
)

// LSSource walks blocks over a liteserver pool (tonutils-go), the LS counterpart
// of ToncenterSource. LS RPCs need a full BlockIDExt (root/file hashes), not just
// (wc,shard,seqno), so the source resolves ids via LookupBlock and caches the
// tops discovered each poll — only catch-up intermediates pay an extra lookup.
//
// Proof checking is disabled (ProofCheckPolicyUnsafe): a TPS meter only needs the
// transaction *count*, and skipping per-tx proof verification is far cheaper and
// more robust on high-throughput blocks.
type LSSource struct {
	api    ton.APIClientWrapped
	desc   string
	pool   *liteclient.ConnectionPool // non-nil only when this source owns it
	bucket *tokenBucket               // outbound RPS cap on every LS request (nil = unlimited)

	mu    sync.Mutex
	cache map[BlockID]*ton.BlockIDExt
}

// NewLSSource builds an LSSource over an already-constructed tonutils API client
// (e.g. backend.LSBackend.API()), so cmd/spam's control run can share the bot's
// liteserver pool. rps caps outbound requests (0 = unlimited) so backfill can't
// overload the liteservers.
func NewLSSource(api ton.APIClientWrapped, description string, rps float64) *LSSource {
	return &LSSource{
		api:    api,
		desc:   description,
		bucket: newTokenBucket(rps),
		cache:  map[BlockID]*ton.BlockIDExt{},
	}
}

// NewLSSourceFromConfig builds an LSSource from a TON global-config JSON file,
// owning its own connection pool (Close it when done). rps caps outbound requests.
func NewLSSourceFromConfig(path string, rps float64) (*LSSource, error) {
	pool := liteclient.NewConnectionPool()
	if err := pool.AddConnectionsFromConfigFile(path); err != nil {
		return nil, fmt.Errorf("ls config %s: %w", path, err)
	}
	api := ton.NewAPIClient(pool, ton.ProofCheckPolicyUnsafe).WithRetry(2)
	s := NewLSSource(api, "ls "+path, rps)
	s.pool = pool
	return s, nil
}

// Close stops the owned connection pool (no-op when the API client was injected).
func (s *LSSource) Close() {
	if s.pool != nil {
		s.pool.Stop()
	}
}

func (s *LSSource) Description() string { return s.desc }

func (s *LSSource) put(ext *ton.BlockIDExt) {
	s.mu.Lock()
	if len(s.cache) > lsCacheCap {
		s.cache = make(map[BlockID]*ton.BlockIDExt, lsCacheCap)
	}
	s.cache[BlockID{ext.Workchain, ext.Shard, ext.SeqNo}] = ext
	s.mu.Unlock()
}

func (s *LSSource) get(id BlockID) *ton.BlockIDExt {
	s.mu.Lock()
	ext := s.cache[id]
	s.mu.Unlock()
	return ext
}

// resolve turns a (wc,shard,seqno) id into a full BlockIDExt, from cache when the
// block was seen this poll, else via LookupBlock.
func (s *LSSource) resolve(ctx context.Context, id BlockID) (*ton.BlockIDExt, error) {
	if ext := s.get(id); ext != nil {
		return ext, nil
	}
	s.bucket.take()
	ext, err := s.api.LookupBlock(ctx, id.Workchain, id.Shard, id.Seqno)
	if err != nil {
		return nil, fmt.Errorf("lookup (%d,%d,%d): %w", id.Workchain, id.Shard, id.Seqno, err)
	}
	s.put(ext)
	return ext, nil
}

func (s *LSSource) LatestMasterchainSeqno(ctx context.Context) (uint32, error) {
	// This is the first Source call of each poll — reset the per-poll cache so the
	// BlockIDExt map cannot grow unbounded across polls.
	s.mu.Lock()
	s.cache = make(map[BlockID]*ton.BlockIDExt, lsCacheCap)
	s.mu.Unlock()

	s.bucket.take()
	mc, err := s.api.GetMasterchainInfo(ctx)
	if err != nil {
		return 0, err
	}
	s.put(mc)
	return mc.SeqNo, nil
}

func (s *LSSource) MasterchainIdentity(ctx context.Context, seqno uint32) (BlockIdentity, error) {
	mc, err := s.resolve(ctx, BlockID{Workchain: mcWorkchain, Shard: mcShard, Seqno: seqno})
	if err != nil {
		return BlockIdentity{}, err
	}
	identity, err := blockIdentity(mc.RootHash, mc.FileHash)
	if err != nil {
		return BlockIdentity{}, fmt.Errorf("MC %d identity: %w", seqno, err)
	}
	return identity, nil
}

func (s *LSSource) Shards(ctx context.Context, mcSeqno uint32) ([]BlockID, error) {
	mc, err := s.resolve(ctx, BlockID{mcWorkchain, mcShard, mcSeqno})
	if err != nil {
		return nil, err
	}
	s.bucket.take()
	shards, err := s.api.GetBlockShardsInfo(ctx, mc)
	if err != nil {
		return nil, err
	}
	if len(shards) == 0 {
		return nil, fmt.Errorf("MC %d returned no shard tops", mcSeqno)
	}
	out := make([]BlockID, 0, len(shards))
	for _, sh := range shards {
		s.put(sh)
		out = append(out, BlockID{sh.Workchain, sh.Shard, sh.SeqNo})
	}
	return out, nil
}

func (s *LSSource) blockHeader(ctx context.Context, id BlockID) (*ton.BlockIDExt, int64, error) {
	ext, err := s.resolve(ctx, id)
	if err != nil {
		return nil, 0, err
	}
	s.bucket.take()
	hdr, err := s.api.GetBlockHeader(ctx, ext)
	if err != nil {
		return nil, 0, fmt.Errorf("block header: %w", err)
	}
	return ext, int64(hdr.GenUtime), nil
}

func (s *LSSource) BlockUtime(ctx context.Context, id BlockID) (int64, error) {
	_, genUtime, err := s.blockHeader(ctx, id)
	return genUtime, err
}

func (s *LSSource) BlockInfo(ctx context.Context, id BlockID) (BlockInfo, error) {
	ext, genUtime, err := s.blockHeader(ctx, id)
	if err != nil {
		return BlockInfo{}, err
	}
	total, partial, err := s.countTx(ctx, ext)
	if err != nil {
		return BlockInfo{}, err
	}
	return BlockInfo{GenUtime: genUtime, TxCount: total, Partial: partial}, nil
}

func (s *LSSource) countTx(ctx context.Context, block *ton.BlockIDExt) (int, bool, error) {
	total := 0
	var after []*ton.TransactionID3
	for page := 0; page < lsTxPages; page++ {
		s.bucket.take()
		list, more, err := s.api.GetBlockTransactionsV2(ctx, block, lsTxPageSize, after...)
		if err != nil {
			return total, false, fmt.Errorf("block txs: %w", err)
		}
		total += len(list)
		if done, partial := classifyTransactionPage(more, len(list)); done {
			return total, partial, nil
		}
		after = []*ton.TransactionID3{list[len(list)-1].ID3()}
	}
	return total, true, nil
}

func classifyTransactionPage(more bool, count int) (done, partial bool) {
	if !more {
		return true, false
	}
	if count == 0 {
		return true, true
	}
	return false, false
}
