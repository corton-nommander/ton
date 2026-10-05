// Package sidechain checks this fork's execution mode before ordinary TVM load.
// The version predicate mirrors validator/impl/{collator-impl.h,validate-query.hpp}.
package sidechain

import (
	"bytes"
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"math/bits"
	"sort"
	"strconv"

	"github.com/xssnick/tonutils-go/liteclient"
	"github.com/xssnick/tonutils-go/tl"
	"github.com/xssnick/tonutils-go/ton"
	"github.com/xssnick/tonutils-go/tvm/cell"
)

const UnsupportedExitCode = 3

var ErrUnsupportedTVM = errors.New("ordinary TVM messages are unsupported in the selected workchain")

type BlockIdentity struct {
	Workchain int32  `json:"workchain"`
	Shard     string `json:"shard"`
	Seqno     uint32 `json:"seqno"`
	RootHash  string `json:"root_hash"`
	FileHash  string `json:"file_hash"`
}

type Report struct {
	LSConfig             string          `json:"ls_config"`
	Liteservers          int             `json:"liteservers"`
	ConfiguredZerostate  *BlockIdentity  `json:"configured_zerostate,omitempty"`
	ObservedZerostate    BlockIdentity   `json:"observed_zerostate"`
	Masterchain          BlockIdentity   `json:"masterchain"`
	Shards               []BlockIdentity `json:"shards"`
	BasechainShardCount  int             `json:"basechain_shard_count"`
	BasechainShardDepths []int           `json:"basechain_shard_depths"`
	GlobalVersion        uint32          `json:"global_version"`
	Capabilities         string          `json:"capabilities"`
	CapabilitiesHex      string          `json:"capabilities_hex"`
	NativeFastPath       bool            `json:"basechain_native_fast_path"`
	NativeTransferRuns   bool            `json:"native_transfer_runs"`
	NativePaymentLanes   bool            `json:"native_payment_lanes"`
	Workchain            int32           `json:"workchain"`
	OrdinaryTVMSupported bool            `json:"ordinary_tvm_supported"`
	Reason               string          `json:"reason"`
	ProofCheckPolicy     string          `json:"proof_check_policy"`
}

// OrdinaryTVMSupported describes this repository's protocol, not stock TON's
// interpretation of global version 14. Unknown workchains are not assumed usable.
func OrdinaryTVMSupported(workchain int32, version uint32) bool {
	return workchain == -1 || (workchain == 0 && version < 14)
}

// DecodeGlobalVersion decodes ConfigParam 8: capabilities#c4 version:uint32
// capabilities:uint64. Ref-bearing, truncated and extended values are rejected.
func DecodeGlobalVersion(param *cell.Cell) (uint32, uint64, error) {
	if param == nil {
		return 0, 0, errors.New("config param 8 is absent")
	}
	s := param.BeginParse()
	if s.BitsLeft() != 104 || s.RefsNum() != 0 {
		return 0, 0, fmt.Errorf("config param 8 must contain 104 bits and no references (got %d bits, %d refs)", s.BitsLeft(), s.RefsNum())
	}
	tag, err := s.LoadUInt(8)
	if err != nil || tag != 0xc4 {
		return 0, 0, fmt.Errorf("config param 8 has invalid GlobalVersion tag %#x", tag)
	}
	version, err := s.LoadUInt(32)
	if err != nil {
		return 0, 0, fmt.Errorf("config param 8 version: %w", err)
	}
	caps, err := s.LoadUInt(64)
	if err != nil {
		return 0, 0, fmt.Errorf("config param 8 capabilities: %w", err)
	}
	return uint32(version), caps, nil
}

func identity(id *ton.BlockIDExt) BlockIdentity {
	return BlockIdentity{Workchain: id.Workchain, Shard: fmt.Sprintf("%016X", uint64(id.Shard)), Seqno: id.SeqNo,
		RootHash: hex.EncodeToString(id.RootHash), FileHash: hex.EncodeToString(id.FileHash)}
}

// Inspect performs only read RPCs. Config and shard proofs are anchored to the
// returned block by tonutils-go; its masterchain proof-chain policy is Unsafe.
// This is a local compatibility preflight, not a consensus finality proof.
func Inspect(ctx context.Context, configPath string, workchain int32) (*Report, error) {
	cfg, err := liteclient.GetConfigFromFile(configPath)
	if err != nil {
		return nil, fmt.Errorf("read liteserver config: %w", err)
	}
	pool := liteclient.NewConnectionPool()
	defer pool.Stop()
	if err := pool.AddConnectionsFromConfig(ctx, cfg); err != nil {
		return nil, fmt.Errorf("connect liteserver: %w", err)
	}
	ctx = pool.StickyContext(ctx)
	api := ton.NewAPIClient(pool, ton.ProofCheckPolicyUnsafe)
	var response tl.Serializable
	if err := pool.QueryLiteserver(ctx, ton.GetMasterchainInf{}, &response); err != nil {
		return nil, fmt.Errorf("masterchain info: %w", err)
	}
	info, ok := response.(ton.MasterchainInfo)
	if !ok || info.Last == nil || info.Init == nil {
		return nil, fmt.Errorf("unexpected masterchain info response: %T", response)
	}
	if info.Last.Workchain != -1 || info.Init.Workchain != -1 || len(info.Init.RootHash) != 32 || len(info.Init.FileHash) != 32 {
		return nil, errors.New("liteserver returned invalid masterchain/zerostate identity")
	}
	configured := cfg.Validator.ZeroState
	report := &Report{LSConfig: configPath, Liteservers: len(cfg.Liteservers), Workchain: workchain,
		Masterchain: identity(info.Last), ProofCheckPolicy: "unsafe_local_preflight", Shards: []BlockIdentity{}, BasechainShardDepths: []int{}}
	report.ObservedZerostate = BlockIdentity{Workchain: info.Init.Workchain, Shard: "8000000000000000", RootHash: hex.EncodeToString(info.Init.RootHash), FileHash: hex.EncodeToString(info.Init.FileHash)}
	if len(configured.RootHash) != 0 || len(configured.FileHash) != 0 {
		if len(configured.RootHash) != 32 || len(configured.FileHash) != 32 {
			return nil, errors.New("configured zerostate hashes must each contain 32 bytes")
		}
		if configured.Workchain != info.Init.Workchain || !bytes.Equal(configured.RootHash, info.Init.RootHash) || !bytes.Equal(configured.FileHash, info.Init.FileHash) {
			return nil, errors.New("liteserver zerostate differs from configured chain identity")
		}
		report.ConfiguredZerostate = &BlockIdentity{Workchain: configured.Workchain, Shard: fmt.Sprintf("%016X", uint64(configured.Shard)), Seqno: configured.SeqNo,
			RootHash: hex.EncodeToString(configured.RootHash), FileHash: hex.EncodeToString(configured.FileHash)}
	}
	config, err := api.GetBlockchainConfig(ctx, info.Last, 8)
	if err != nil {
		return nil, fmt.Errorf("read config param 8: %w", err)
	}
	version, caps, err := DecodeGlobalVersion(config.Get(8))
	if err != nil {
		return nil, err
	}
	report.GlobalVersion = version
	report.Capabilities = strconv.FormatUint(caps, 10)
	report.CapabilitiesHex = fmt.Sprintf("0x%016X", caps)
	report.NativeFastPath = version >= 14
	report.NativeTransferRuns = version >= 15 && caps&1024 != 0
	report.NativePaymentLanes = version >= 16 && caps&3072 == 3072
	report.OrdinaryTVMSupported = OrdinaryTVMSupported(workchain, version)
	switch {
	case workchain == 0 && version >= 14:
		report.Reason = "This fork's basechain uses native balance/nonce execution at global version >= 14; ordinary TVM externals and internal message queues are disabled."
	case workchain == -1:
		report.Reason = "Masterchain retains ordinary TVM execution. This is a separate workload from optimized basechain native transfers; all generated messages must remain in masterchain."
	case workchain == 0:
		report.Reason = "Basechain global version < 14 selects the ordinary TVM execution path."
	default:
		report.Reason = "Only workchain 0 and masterchain -1 are supported by this utility."
	}
	shards, err := api.GetBlockShardsInfo(ctx, info.Last)
	if err != nil {
		return nil, fmt.Errorf("read shard topology: %w", err)
	}
	depths := map[int]bool{}
	for _, shard := range shards {
		report.Shards = append(report.Shards, identity(shard))
		if shard.Workchain == 0 {
			report.BasechainShardCount++
			if shard.Shard != 0 {
				depths[63-bits.TrailingZeros64(uint64(shard.Shard))] = true
			}
		}
	}
	sort.Slice(report.Shards, func(i, j int) bool {
		if report.Shards[i].Workchain != report.Shards[j].Workchain {
			return report.Shards[i].Workchain < report.Shards[j].Workchain
		}
		return report.Shards[i].Shard < report.Shards[j].Shard
	})
	for depth := range depths {
		report.BasechainShardDepths = append(report.BasechainShardDepths, depth)
	}
	sort.Ints(report.BasechainShardDepths)
	return report, nil
}

// RequireTVM returns the report even when its selected workchain is unsupported.
func RequireTVM(ctx context.Context, configPath string, workchain int32) (*Report, error) {
	report, err := Inspect(ctx, configPath, workchain)
	if err != nil {
		return nil, err
	}
	if !report.OrdinaryTVMSupported {
		return report, fmt.Errorf("%w: %s", ErrUnsupportedTVM, report.Reason)
	}
	return report, nil
}
