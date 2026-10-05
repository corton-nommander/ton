// FatReproduce pool deploy / teardown helpers (see contracts/FatReproduce.tolk).
//
// The whole pool [0, count] is stood up by ONE signed external to wallet 0 that (a) makes wallet 0
// self-build its own cell chain, and (b) fires a FatSpread at wallet 1 which self-replicates the rest
// on-chain; every account then builds its own large, per-account-UNIQUE cell chain (FatBuild grow loop).
// No minter / no jetton — a FatReproduce address depends only on (subwalletId, publicKey, code).
//
// Data layout (must match FatReproduceStorage.toCell(), so pool addresses match the on-chain contract):
//   seqno:uint32 | subwalletId:uint32 | publicKey:uint256 | cellsBuilt:uint32 | batches:dict<uint32,cell>
// The address preimage is the seqno=0, cellsBuilt=0, EMPTY-dict initial state.

import "@acton/build"
import "@acton/crypto"
import "@acton/env"
import "@acton/fs"
import "@acton/io"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "@acton/types/big_array"
import "@contracts/FatReproduce"
import "@wrappers/FatReproduce.gen"

const VALID_UNTIL = 0xFFFFFFFF

// ---- pool keypair: raw 32-byte ed25519 seed in a key file (default fat-pool.key; override with
//      FAT_KEY_FILE so several size-tiers can each own a distinct pool/key — sizes don't change the
//      address, only the key does, so one key per size-tier avoids address collisions).

fun fatKeyFile(): string {
    return env<string>("FAT_KEY_FILE") ?? "fat-pool.key";
}

fun loadFatPoolKeyPair(): KeyPair {
    val kf = fatKeyFile();
    val raw = fs.readBytes(kf);
    if (raw != null) {
        var sl = raw!;
        if (sl.remainingBitsCount() >= 256) {
            return crypto.seedToKeyPair(sl.loadUint(256));
        }
        println("  ##### WARN {} is malformed (<32 bytes) -> generating a fresh key", kf);
    }
    val kp = crypto.generateKeyPair();
    val saved = fs.writeBytes(kf, beginCell().storeUint(kp.privateKey, 256).endCell().beginParse());
    if (saved) {
        println("  ##### NOTE generated a NEW fat pool key -> saved to {}", kf);
    } else {
        println("  ##### WARN could not write {} — THIS RUN'S KEY IS NOT PERSISTED", kf);
    }
    return kp;
}

// ---- address helpers (mirror the contract's buildChildData / siblingStateInit) ----

fun fatStateInit(subwallet: int, pubkey: int): AutoDeployAddress {
    return {
        workchain: BASECHAIN,
        stateInit: ContractState {
            code: build("FatReproduce"),
            data: FatReproduceStorage {
                seqno: 0,
                subwalletId: subwallet,
                publicKey: pubkey,
                cellsBuilt: 0,
                batches: [],
            }.toCell(),
        },
    };
}

fun fatAddress(subwallet: int, pubkey: int): address {
    return fatStateInit(subwallet, pubkey).calculateAddress();
}

// ---- kickoff external ----

// Signed external (sig-first) that: (1) makes wallet 0 self-send its own FatBuild, and (2) fires the
// FatSpread that deploys wallet 1 and drives the whole chain. Layout matches WalletSpam / the Go signer:
//   signature(512) || subwallet(32)|validUntil(32)|seqno(32)|(sendMode(8)|^msg)*
fun buildFatKickoffExternal(
    kp: KeyPair,
    seqno: int,
    wallet0Addr: address,
    count: int,
    chunk: int,
    walletValue: coins,
    chainCells: int,
    buildStep: int,
    buildValue: coins,
    reproFunding: coins,
): cell {
    val selfBuild = createMessage({
        bounce: BounceMode.NoBounce,
        value: buildValue,
        dest: wallet0Addr,
        body: FatBuild { remaining: chainCells, buildStep, buildValue },
    }).messageCell;

    val reproMsg = createMessage({
        bounce: BounceMode.Only256BitsOfBody,
        value: reproFunding,
        dest: fatStateInit(1, kp.publicKey),
        body: FatSpread {
            lastSubwallet: count,
            fromSubwallet: 0,
            chunk,
            walletValue,
            chainCells,
            buildStep,
            buildValue,
        },
    }).messageCell;

    val signed = beginCell()
        .storeUint(0, 32)
        .storeUint(VALID_UNTIL, 32)
        .storeUint(seqno, 32)
        .storeUint(SEND_MODE_PAY_FEES_SEPARATELY, 8)
        .storeRef(selfBuild)
        .storeUint(SEND_MODE_REGULAR, 8)
        .storeRef(reproMsg)
        .endCell();
    val sig = crypto.rawSign(kp.privateKey, signed.hash());
    return beginCell().storeSlice(sig).storeSlice(signed.beginParse()).endCell();
}

// ---- deploy the whole pool ----

// perWallet = walletValue + feeSlack; wallet 0 is funded (count+1)*perWallet and forwards count*perWallet
// down the reproduce chain. walletValue must cover each account's whole build (grow-loop gas) plus its
// resting balance. Returns wallet 0's address.
fun deployFatPool(
    deployer: ActonWallet,
    count: int,
    walletValue: coins,
    buildValue: coins,
    feeSlack: coins,
    chunk: int,
    chainCells: int,
    buildStep: int,
): address {
    val kp = loadFatPoolKeyPair();
    val pubkey = kp.publicKey;
    val code = build("FatReproduce");

    val perWallet = walletValue + feeSlack;
    val wallet0Funding = (count + 1) * perWallet;
    val reproFunding = count * perWallet;

    val wallet0 = fatStateInit(0, pubkey);
    val wallet0Addr = wallet0.calculateAddress();
    net.registerCodeCell(code, "FatReproduce");
    net.registerAddress(wallet0Addr, "FatReproduce#0");

    println("FAT CODE_HASH={}", code.hash());
    println("FAT POOL_PUBKEY={}", pubkey);
    println("FAT WALLET0={}", wallet0Addr);
    println("FAT PARAMS count={} chunk={} chainCells={} buildStep={}", count, chunk, chainCells, buildStep);
    println(
        "FAT FUNDING wallet0={} repro={} perWallet={} walletValue={} buildValue={}",
        wallet0Funding,
        reproFunding,
        perWallet,
        walletValue,
        buildValue,
    );

    // 1) deploy wallet 0
    net
        .send(
            deployer.address,
            createMessage({ bounce: BounceMode.NoBounce, value: wallet0Funding, dest: wallet0 }),
        )
        .waitForFirstTransaction();

    if (count <= 0) {
        // Only wallet 0: still kick its own build.
        val ext0 = buildFatKickoffExternal(
            kp, 0, wallet0Addr, 0, chunk, walletValue, chainCells, buildStep, buildValue, 0,
        );
        net.sendExternal(net.createExternalMessage(wallet0Addr, ext0)).waitForTrace();
        println("FAT POOL_SIZE=1 (count<=0, only wallet 0)");
        return wallet0Addr;
    }

    // 2) kickoff: wallet0 self-builds + FatSpread to wallet1 (self-replicates the rest on-chain)
    val extBody = buildFatKickoffExternal(
        kp, 0, wallet0Addr, count, chunk, walletValue, chainCells, buildStep, buildValue, reproFunding,
    );
    val extResult = net.sendExternal(net.createExternalMessage(wallet0Addr, extBody));
    if (!extResult.isAccepted()) {
        println("FAT KICKOFF_ACCEPTED=false ERROR={}", extResult.error!.message);
        return wallet0Addr;
    }
    extResult.waitForTrace();

    // 3) verify — but only the whole pool for SMALL runs; for big pools the reproduce + grow-loop cascade
    //    spans many blocks past waitForTrace, and checking all count+1 wallets is thousands of RPC calls, so
    //    just confirm the kickoff landed and hand off to scripts/_fat_status.tolk (sampled) for monitoring.
    if (count > 200) {
        println("FAT KICKOFF_SENT count={} — monitor with _fat_status.tolk (sampled)", count);
        return wallet0Addr;
    }
    var deployed = 0;
    var fullyBuilt = 0;
    var i = 0;
    while (i <= count) {
        val addr = fatAddress(i, pubkey);
        if (scripts.isContractDeployed(addr)) {
            deployed += 1;
            if (FatReproduce.fromAddress(addr).getCellsBuilt() >= chainCells) {
                fullyBuilt += 1;
            }
        }
        i += 1;
    }
    println("FAT POOL_DEPLOYED={}/{} FULLY_BUILT={}/{}", deployed, count + 1, fullyBuilt, count + 1);
    return wallet0Addr;
}

// ---- teardown ----

// One signed external on wallet 0: wallet 0 rolls its whole balance to wallet 1 via CARRY_ALL_BALANCE|
// DESTROY (its own self-destruct), carrying the FatDestroy that drives the linear roll-up to `collector`.
fun buildFatDestroyExternal(kp: KeyPair, seqno: int, count: int, collector: address): cell {
    var b = beginCell().storeUint(0, 32).storeUint(VALID_UNTIL, 32).storeUint(seqno, 32);
    if (count <= 0) {
        val sweep = createMessage({ bounce: BounceMode.NoBounce, value: 0, dest: collector }).messageCell;
        b = b.storeUint(SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_DESTROY, 8).storeRef(sweep);
    } else {
        val destroyMsg = createMessage({
            bounce: BounceMode.NoBounce,
            value: 0,
            dest: fatAddress(1, kp.publicKey),
            body: FatDestroy { lastSubwallet: count, collector },
        }).messageCell;
        b = b.storeUint(SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_DESTROY, 8).storeRef(destroyMsg);
    }
    val signed = b.endCell();
    val sig = crypto.rawSign(kp.privateKey, signed.hash());
    return beginCell().storeSlice(sig).storeSlice(signed.beginParse()).endCell();
}

fun destroyFatPool(count: int, collector: address): void {
    val kp = loadFatPoolKeyPair();
    val pubkey = kp.publicKey;
    val wallet0Addr = fatAddress(0, pubkey);
    println("FAT DESTROY COLLECTOR={} COUNT={}", collector, count);
    if (!scripts.isContractDeployed(wallet0Addr)) {
        println("FAT DESTROY SKIPPED wallet0 not deployed");
        return;
    }
    val w0Seqno = FatReproduce.fromAddress(wallet0Addr).seqno();
    val res = net.sendExternal(
        net.createExternalMessage(wallet0Addr, buildFatDestroyExternal(kp, w0Seqno, count, collector)),
    );
    if (!res.isAccepted()) {
        println("FAT DESTROY NOT_ACCEPTED err={}", res.error!.message);
        return;
    }
    res.waitForTrace();

    var alive = 0;
    var j = 0;
    while (j <= count) {
        if (scripts.isContractDeployed(fatAddress(j, pubkey))) {
            alive += 1;
        }
        j += 1;
    }
    println("FAT DESTROY DONE alive={} (should be 0)", alive);
}
