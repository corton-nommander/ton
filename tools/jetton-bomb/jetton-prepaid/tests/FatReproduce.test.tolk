import "@acton/build"
import "@acton/crypto"
import "@acton/emulation/network"
import "@acton/emulation/testing"
import "@acton/emulation/config"
import "@acton/testing/expect"

import "@contracts/FatReproduce"
import "@wrappers/FatReproduce.gen"

import "test-utils"

// Small pool, tiny chunk so a modest N still exercises the multi-hop chain (as in WalletSpam.test):
// N=5, chunk=2  =>  wallet1 drives {2 leaf, 3 driver}; wallet3 drives {4 leaf, 5 driver/terminal}.
// CHAIN_CELLS > BUILD_STEP so the per-account grow loop runs (10 cells in 4+4+2 = 3 FatBuild txs).
const POOL_N = 5
const POOL_CHUNK = 2
const WALLET_VALUE = grams("1") // grams each pool account keeps / self-funds its build from
const BUILD_VALUE = grams("0.3") // grams each FatBuild self-message carries
const CHAIN_CELLS = 10
const BUILD_STEP = 4
const WALLET0_FUND = grams("100")
const REPRO_FUNDING = grams("60")
const FAR_FUTURE = 0xFFFFFFFF

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------

// reproduce: the whole pool 0..N comes up as one chain, and every account builds its full cell chain
// in-chain (get_cells_built == CHAIN_CELLS), across the grow loop.
get fun `test fat reproduce: chain deploys every account and each builds its full chain`() {
    val kp = crypto.generateKeyPair();
    val wallet0 = deployWallet0(kp);

    wallet0.sendAnyExternal(buildKickoffExternal(kp, 0)).unwrap();

    var i = 0;
    while (i <= POOL_N) {
        val addr = poolAddr(i, kp);
        expect(testing.isDeployed(addr)).toEqual(true);
        expect(FatReproduce.fromAddress(addr).getCellsBuilt()).toEqual(CHAIN_CELLS);
        i += 1;
    }

    // seqno bumped exactly once on the entry wallet
    expect(wallet0.seqno()).toEqual(1);
}

// destroy: one signed external on wallet 0 drives the linear roll-up — every account self-destructs and
// its whole balance rolls to `collector`. The whole pool (including its big state) vanishes.
get fun `test fat destroy: roll-up wipes the whole pool into collector`() {
    val kp = crypto.generateKeyPair();
    val collector = testing.treasury("fat-collector");
    val wallet0 = deployWallet0(kp);
    wallet0.sendAnyExternal(buildKickoffExternal(kp, 0)).unwrap();

    val collectorBefore = testing.getAccountBalance(collector.address);

    // seqno is now 1 (deploy kickoff used seqno 0)
    wallet0.sendAnyExternal(buildDestroyExternal(kp, 1, collector.address)).unwrap();

    var alive = 0;
    var i = 0;
    while (i <= POOL_N) {
        if (testing.isDeployed(poolAddr(i, kp))) {
            alive += 1;
        }
        i += 1;
    }
    expect(alive).toEqual(0);
    expect(testing.getAccountBalance(collector.address) > collectorBefore).toEqual(true);
}

// external guards: valid external accepted + bumps seqno; wrong seqno / subwallet / expiry / signature all
// rejected before acceptance; state survives the rejects.
get fun `test fat external: seqno, subwallet, signature guards`() {
    val kp = crypto.generateKeyPair();
    val wallet0 = deployWallet0(kp);

    expect(wallet0.sendAnyExternal(bareExternal(kp, 0, 0, FAR_FUTURE))).toBeAccepted();
    expect(wallet0.seqno()).toEqual(1);

    expect(wallet0.sendAnyExternal(bareExternal(kp, 0, 0, FAR_FUTURE))).toBeNotAccepted(); // stale seqno
    expect(wallet0.sendAnyExternal(bareExternal(kp, 9, 1, FAR_FUTURE))).toBeNotAccepted(); // wrong subwallet
    expect(wallet0.sendAnyExternal(bareExternal(kp, 0, 1, 1))).toBeNotAccepted(); // expired
    val wrongKp = crypto.generateKeyPair();
    expect(
        wallet0.sendAnyExternal(bareExternalSignedBy(wrongKp, 0, 1, FAR_FUTURE)),
    ).toBeNotAccepted(); // bad signature

    expect(wallet0.sendAnyExternal(bareExternal(kp, 0, 1, FAR_FUTURE))).toBeAccepted();
    expect(wallet0.seqno()).toEqual(2);
}

// internal auth: a FatReproduce not from the claimed parent driver is rejected (401).
get fun `test fat reproduce: unauthorized sender rejected`() {
    val kp = crypto.generateKeyPair();
    val stranger = testing.treasury("stranger");
    val wallet0 = deployWallet0(kp);
    wallet0.sendAnyExternal(buildKickoffExternal(kp, 0)).unwrap();

    val wallet1 = FatReproduce.fromAddress(poolAddr(1, kp));
    val res = wallet1.sendFatSpread(
        stranger.address, // not wallet 0 -> fails the sibling check
        POOL_N,
        0,
        POOL_CHUNK,
        WALLET_VALUE,
        CHAIN_CELLS,
        BUILD_STEP,
        BUILD_VALUE,
        { value: grams("1"), bounce: true },
    );
    expect(res).toHaveFailedTx<FatSpread>({
        from: stranger.address,
        to: wallet1.address,
        exitCode: 401,
    });
}

// build auth: a FatBuild from anyone but the account itself is rejected (401).
get fun `test fat build: only self may build`() {
    val kp = crypto.generateKeyPair();
    val stranger = testing.treasury("stranger2");
    val wallet0 = deployWallet0(kp);

    val res = wallet0.sendFatBuild(
        stranger.address, // not wallet 0 itself -> 401
        CHAIN_CELLS,
        BUILD_STEP,
        BUILD_VALUE,
        { value: grams("1"), bounce: true },
    );
    expect(res).toHaveFailedTx<FatBuild>({
        from: stranger.address,
        to: wallet0.address,
        exitCode: 401,
    });
}

// chunk clamp: an over-limit chunk aborts the first driver (BadChunk=38); nothing below wallet 1 deploys.
get fun `test fat reproduce: over-limit chunk is rejected`() {
    val kp = crypto.generateKeyPair();
    val wallet0 = deployWallet0(kp);

    val res = wallet0.sendAnyExternal(buildKickoffExternalChunk(kp, 0, 254)).unwrap();

    expect(res).toHaveFailedTx<FatSpread>({
        to: poolAddr(1, kp),
        exitCode: 38,
    });
    expect(testing.isDeployed(poolAddr(2, kp))).toEqual(false);
}

// multi-sub-batch: a single FatBuild whose buildStep > MAX_BATCH_DEPTH (150) builds ALL its cells in ONE
// tx (split into depth-safe sub-chains), not a grow loop. 180 cells = 150 + 30 in one tx. This is the path
// that, with a raised gas_limit, collapses a whole account to one build tx (one storage-stat touch).
get fun `test fat build: builds more than MAX_BATCH_DEPTH cells in one tx`() {
    val kp = crypto.generateKeyPair();
    val wallet0 = deployWallet0(kp);

    val selfBuild = createMessage({
        bounce: BounceMode.NoBounce,
        value: grams("5"),
        dest: poolAddr(0, kp),
        body: FatBuild { remaining: 180, buildStep: 180, buildValue: grams("5") },
    }).messageCell;
    val signed = beginCell()
        .storeUint(0, 32)
        .storeUint(FAR_FUTURE, 32)
        .storeUint(0, 32)
        .storeUint(SEND_MODE_PAY_FEES_SEPARATELY, 8)
        .storeRef(selfBuild)
        .endCell();
    wallet0.sendAnyExternal(finalizeExternal(kp, signed)).unwrap();

    // 180 > 150 => 2 sub-batches, but a SINGLE FatBuild tx (remaining hit 0, no grow-loop self-send)
    expect(wallet0.getCellsBuilt()).toEqual(180);
}

// THE config-accelerated path: with a raised ConfigParam-21 gas_limit, ONE FatBuild builds a whole 100 KB
// account (833 cells, buildStep = chainCells) in a SINGLE tx = one storage-stat touch. Validated by
// bumping the emulator's basechain gas_limit to 100M (what we'll ask the colleague to set on testnet).
get fun `test fat build: full 100KB account in ONE tx under raised gas_limit`() {
    var config = testing.getConfig();
    config.setGasPrices(
        GasPrices {
            flatGasLimit: 100,
            flatGasPrice: 1000000,
            other: GasPricesExtended {
                gasPrice: 1000,
                gasLimit: 100_000_000, // raised from 1M
                specialGasLimit: 100_000_000,
                gasCredit: 10000,
                blockGasLimit: 1_000_000_000,
                freezeDueLimit: 100000000,
                deleteDueLimit: 1000000000,
            },
        },
        BASECHAIN,
    );
    expect(testing.setConfig(config)).toEqual(true);

    val kp = crypto.generateKeyPair();
    val wallet0 = deployWallet0(kp);

    // ONE FatBuild, buildStep == chainCells == 833: ~4.5M gas, ~6 depth-safe sub-batches, single tx.
    val selfBuild = createMessage({
        bounce: BounceMode.NoBounce,
        value: grams("10"),
        dest: poolAddr(0, kp),
        body: FatBuild { remaining: 833, buildStep: 833, buildValue: grams("10") },
    }).messageCell;
    val signed = beginCell()
        .storeUint(0, 32)
        .storeUint(FAR_FUTURE, 32)
        .storeUint(0, 32)
        .storeUint(SEND_MODE_PAY_FEES_SEPARATELY, 8)
        .storeRef(selfBuild)
        .endCell();
    wallet0.sendAnyExternal(finalizeExternal(kp, signed)).unwrap();

    expect(wallet0.getCellsBuilt()).toEqual(833);
}

// kill-switch: config -137 present with first 16 bits == 1 halts the signed kickoff (and thus every
// FatSpread/FatBuild, which share assertNotKilled). Absent config = normal run (covered by the other tests).
get fun `test fat kill-switch: config -137 halts kickoff`() {
    var config = testing.getConfig();
    // The emulation config map keys are uint32, so pass -137 as its 32-bit two's-complement (0xFFFFFF77) —
    // the same 32-bit key the contract's CONFIGOPTPARAM(-137) reads on the real chain.
    config.setParamRaw(0xFFFFFF77, beginCell().storeUint(1, 16).endCell());
    expect(testing.setConfig(config)).toEqual(true);

    val kp = crypto.generateKeyPair();
    val wallet0 = deployWallet0(kp);
    // killed -> assertNotKilled throws (666) before acceptExternalMessage, so the kickoff is rejected
    expect(wallet0.sendAnyExternal(buildKickoffExternal(kp, 0))).toBeNotAccepted();
    // and nothing spread below wallet 0
    expect(testing.isDeployed(poolAddr(1, kp))).toEqual(false);
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

fun poolStateInit(subwallet: int, publicKey: uint256): AutoDeployAddress {
    return {
        workchain: BASECHAIN,
        stateInit: ContractState {
            code: build("FatReproduce"),
            data: FatReproduceStorage {
                seqno: 0,
                subwalletId: subwallet,
                publicKey,
                cellsBuilt: 0,
                batches: [],
            }.toCell(),
        },
    };
}

fun poolAddr(subwallet: int, kp: KeyPair): address {
    return poolStateInit(subwallet, kp.publicKey).calculateAddress();
}

fun deployWallet0(kp: KeyPair): FatReproduce {
    val deployer = testing.treasury("fat-deployer");
    val wallet0 = FatReproduce.fromStorage({
        seqno: 0,
        subwalletId: 0,
        publicKey: kp.publicKey,
        cellsBuilt: 0,
        batches: [],
    });
    val res = wallet0.deploy(deployer.address, { value: WALLET0_FUND, bounce: false });
    expect(res).toHaveSuccessfulDeploy({ to: wallet0.address });
    return wallet0;
}

fun finalizeExternal(kp: KeyPair, signed: cell): cell {
    val sig = crypto.rawSign(kp.privateKey, signed.hash());
    return beginCell().storeSlice(sig).storeSlice(signed.beginParse()).endCell();
}

fun bareExternalSignedBy(kp: KeyPair, subwallet: int, seqno: int, validUntil: int): cell {
    val signed = beginCell()
        .storeUint(subwallet, 32)
        .storeUint(validUntil, 32)
        .storeUint(seqno, 32)
        .endCell();
    return finalizeExternal(kp, signed);
}

fun bareExternal(kp: KeyPair, subwallet: int, seqno: int, validUntil: int): cell {
    return bareExternalSignedBy(kp, subwallet, seqno, validUntil);
}

// Kickoff external: wallet 0 self-sends its own FatBuild, and fires the FatReproduce that deploys wallet 1
// and drives the whole chain (each deployed account then self-builds).
fun buildKickoffExternalChunk(kp: KeyPair, seqno: int, chunk: int): cell {
    val w0 = poolAddr(0, kp);
    val selfBuild = createMessage({
        bounce: BounceMode.NoBounce,
        value: BUILD_VALUE,
        dest: w0,
        body: FatBuild { remaining: CHAIN_CELLS, buildStep: BUILD_STEP, buildValue: BUILD_VALUE },
    }).messageCell;
    val reproMsg = createMessage({
        bounce: BounceMode.Only256BitsOfBody,
        value: REPRO_FUNDING,
        dest: poolStateInit(1, kp.publicKey),
        body: FatSpread {
            lastSubwallet: POOL_N,
            fromSubwallet: 0,
            chunk,
            walletValue: WALLET_VALUE,
            chainCells: CHAIN_CELLS,
            buildStep: BUILD_STEP,
            buildValue: BUILD_VALUE,
        },
    }).messageCell;

    val signed = beginCell()
        .storeUint(0, 32)
        .storeUint(FAR_FUTURE, 32)
        .storeUint(seqno, 32)
        .storeUint(SEND_MODE_PAY_FEES_SEPARATELY, 8)
        .storeRef(selfBuild)
        .storeUint(SEND_MODE_REGULAR, 8)
        .storeRef(reproMsg)
        .endCell();
    return finalizeExternal(kp, signed);
}

fun buildKickoffExternal(kp: KeyPair, seqno: int): cell {
    return buildKickoffExternalChunk(kp, seqno, POOL_CHUNK);
}

// Teardown external: wallet 0 rolls its whole balance to wallet 1 via CARRY_ALL_BALANCE|DESTROY (its own
// self-destruct), carrying the FatDestroy that drives the linear roll-up of the rest to `collector`.
fun buildDestroyExternal(kp: KeyPair, seqno: int, collector: address): cell {
    val destroyChainMsg = createMessage({
        bounce: BounceMode.NoBounce,
        value: 0,
        dest: poolAddr(1, kp),
        body: FatDestroy { lastSubwallet: POOL_N, collector },
    }).messageCell;

    val signed = beginCell()
        .storeUint(0, 32)
        .storeUint(FAR_FUTURE, 32)
        .storeUint(seqno, 32)
        .storeUint(SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_DESTROY, 8)
        .storeRef(destroyChainMsg)
        .endCell();
    return finalizeExternal(kp, signed);
}
