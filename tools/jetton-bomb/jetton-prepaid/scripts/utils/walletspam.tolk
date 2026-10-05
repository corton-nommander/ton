// WalletSpam pool deploy helpers (native Tolk WalletSpam — see contracts/WalletSpam.tolk).
//
// The whole pool [0, count] is stood up by ONE signed external to wallet 0 that wraps a Reproduce
// aimed at wallet 1 (+ wallet 0's own jetton deploy). Each wallet then deploys its own prepaid jetton
// wallet in-chain (when deployJetton) and replicates the rest of the pool in chunks. No FunC BoC embed
// and no spam_msg anymore — WalletSpam is compiled from Tolk via `build("WalletSpam")`.
//
// Data layout (must match WalletSpamStorage.toCell(), so pool addresses match the on-chain contract):
//   seqno:uint32 | subwalletId:uint32 | publicKey:uint256 | minterAddress:address | ^jettonWalletCode
// StateInit is the canonical `0 0 1^code 1^data 0`, so calculateAddress() == the on-chain address.

import "@acton/build"
import "@acton/crypto"
import "@acton/env"
import "@acton/fs"
import "@acton/io"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "@contracts/WalletSpam"
import "@contracts/jetton-utils"
import "@contracts/messages"
import "@wrappers/WalletSpam.gen"

// Far-future valid_until: WalletSpam rejects `valid_until <= now()`. A fixed max keeps the signed
// external valid in both emulation and broadcast; seqno (single-use) prevents replay.
const VALID_UNTIL = 0xFFFFFFFF

// Per-wallet slack folded into the funding so forward fees + gas along the whole chain are covered
// (the last wallet keeps whatever tail is left over). MAINNET cascade cost is only ~0.001 GRAM/wallet
// (measured: a live mainnet 5k pool at 0.01 finished with ~43 of its 50 GRAM slack still unused in the
// terminal wallet), so 0.015 here = 1.5x the already-safe 0.01. NOTE devnet fees are ~20x higher (~0.02)
// so the Go devnet deploy (cmd/deploy) uses a much fatter 0.1 — don't copy that here (mainnet grams cost).
const FEE_SLACK_PER_WALLET = 15000000 // 0.015 GRAM (mainnet)

// ---- WalletSpam address helpers (mirror the contract's buildChildData / siblingStateInit) ----

fun walletSpamStateInit(
    subwallet: int,
    pubkey: int,
    minter: address,
    jwCode: cell,
): AutoDeployAddress {
    return {
        workchain: BASECHAIN,
        stateInit: ContractState {
            code: build("WalletSpam"),
            data: WalletSpamStorage {
                seqno: 0,
                subwalletId: subwallet,
                publicKey: pubkey,
                minterAddress: minter,
                jettonWalletCode: jwCode,
            }.toCell(),
        },
    };
}

fun walletSpamAddress(subwallet: int, pubkey: int, minter: address, jwCode: cell): address {
    return walletSpamStateInit(subwallet, pubkey, minter, jwCode).calculateAddress();
}

// ---- pool keypair: raw 32-byte ed25519 seed in pool.key ----

// The shared pool keypair is a raw 32-byte ed25519 seed stored in `pool.key`, in the ACTON PROJECT DIR
// (jetton-prepaid/) — acton's fs is sandboxed there, so `../`, absolute paths and escaping symlinks all
// return null; the file must physically live in jetton-prepaid/. The Go bot reads the SAME 32 bytes via
// ed25519.NewKeyFromSeed, so the derived pubkey and every pool address match byte-for-byte
// (crypto.seedToKeyPair == ed25519.NewKeyFromSeed, verified). If pool.key is absent it GENERATES a fresh
// seed and SAVES it (never a lost throwaway) — mirror pool.key to wherever the Go bot reads it.
fun loadPoolKeyPair(): KeyPair {
    // Default pool.key (the Go bot reads the same file); override with POOL_KEY_FILE for a throwaway/test
    // pool on a separate key (e.g. a 1k rehearsal that must not collide with the real pool's address space).
    val keyFile = env<string>("POOL_KEY_FILE") ?? "pool.key";
    val raw = fs.readBytes(keyFile);
    if (raw != null) {
        var sl = raw!;
        if (sl.remainingBitsCount() >= 256) {
            return crypto.seedToKeyPair(sl.loadUint(256));
        }
        println("  ##### WARN {} is malformed (<32 bytes) -> generating a fresh key", keyFile);
    }

    // Generate + persist a fresh seed. crypto.generateKeyPair().privateKey IS the 32-byte seed, and
    // seedToKeyPair(it) reproduces this same keypair, so a later reload of the key file is deterministic.
    val kp = crypto.generateKeyPair();
    val saved = fs.writeBytes(
        keyFile,
        beginCell().storeUint(kp.privateKey, 256).endCell().beginParse(),
    );
    if (saved) {
        println("  ##### NOTE generated a NEW pool key -> saved to jetton-prepaid/{} (32-byte", keyFile);
        println("  ##### ed25519 seed). Point the Go bot at the same file for address parity.");
    } else {
        println("  ##### WARN could not write {} — THIS RUN'S KEY IS NOT PERSISTED (can't reuse it)", keyFile);
    }
    return kp;
}

// ---- kickoff external ----

// Wrap a signed external (sig-first) around wallet 0's own jetton deploy (when deployJetton) plus the
// Reproduce that deploys wallet 1 and drives the chain. Matches WalletSpam.onExternalMessage and the
// Go BuildExternalBodyWithMessage: signature(512) || subwallet|validUntil|seqno|(sendMode|^msg)*.
fun buildKickoffExternal(
    kp: KeyPair,
    seqno: int,
    wallet0Addr: address,
    minter: address,
    jwCode: cell,
    count: int,
    chunk: int,
    deployJetton: bool,
    walletValue: coins,
    jettonValue: coins,
    reproFunding: coins,
): cell {
    val reproMsg = createMessage({
        bounce: BounceMode.Only256BitsOfBody,
        value: reproFunding,
        dest: walletSpamStateInit(1, kp.publicKey, minter, jwCode),
        body: Reproduce {
            lastSubwallet: count,
            fromSubwallet: 0,
            chunk,
            deployJetton,
            walletValue,
            jettonValue,
        },
    }).messageCell;

    var b = beginCell()
        .storeUint(0, 32) // subwallet 0
        .storeUint(VALID_UNTIL, 32)
        .storeUint(seqno, 32);

    if (deployJetton) {
        // wallet 0 is deployed by the deployer (not via Reproduce), so it deploys its own jetton
        // wallet here in the kickoff wrapper.
        val w0Jetton = createMessage({
            bounce: BounceMode.NoBounce,
            value: jettonValue,
            dest: calcDeployedJettonWallet(wallet0Addr, minter, jwCode),
            body: TopUpGrams {},
        }).messageCell;
        b = b.storeUint(SEND_MODE_PAY_FEES_SEPARATELY, 8).storeRef(w0Jetton);
    }
    b = b.storeUint(SEND_MODE_REGULAR, 8).storeRef(reproMsg);

    val signed = b.endCell();
    val sig = crypto.rawSign(kp.privateKey, signed.hash());
    return beginCell().storeSlice(sig).storeSlice(signed.beginParse()).endCell();
}

// ---- deploy the whole pool ----

// Deploy WalletSpam pool [0, count] from `deployer`, funding it from a single per-wallet lever:
//   perWallet = walletValue + FEE_SLACK_PER_WALLET
// walletValue is the TOTAL grams each wallet consumes: it keeps walletValue-jettonValue for itself and
// funds its own jetton wallet with jettonValue out of that SAME walletValue (a leaf receives walletValue
// and spends jettonValue on its jetton; a driver reserves walletValue-jettonValue and its jetton holds
// jettonValue). So the jetton is NOT an extra cost — adding jettonValue here again would over-fund the
// pool by ~jettonValue per wallet (the "1500 instead of 1000" overshoot).
// wallet 0 is funded with (count+1)*perWallet and forwards count*perWallet down the reproduce chain.
// Returns wallet 0's address.
fun deployWalletSpamPool(
    deployer: ActonWallet,
    count: int,
    minter: address,
    jwCode: cell,
    walletValue: coins,
    jettonValue: coins,
    feeSlack: coins,
    chunk: int,
    deployJetton: bool,
): address {
    val kp = loadPoolKeyPair();
    val pubkey = kp.publicKey;
    val code = build("WalletSpam");

    // feeSlack (FEE_SLACK env, default FEE_SLACK_PER_WALLET = 0.015) is the ONLY headroom on top of
    // walletValue. Barely any is burned on MAINNET (~0.001 GRAM/wallet of cascade fees); the rest rolls
    // forward and piles up in the terminal (tail) wallet, recoverable by DestroyChain. Too LOW starves the
    // last driver → the tail never deploys. (On DEVNET fees are ~20x higher, so cmd/deploy uses 0.1 there.)
    val perWallet = walletValue + feeSlack;
    val wallet0Funding = (count + 1) * perWallet;
    val reproFunding = count * perWallet;

    val wallet0 = walletSpamStateInit(0, pubkey, minter, jwCode);
    val wallet0Addr = wallet0.calculateAddress();
    net.registerCodeCell(code, "WalletSpam");
    net.registerAddress(wallet0Addr, "WalletSpam#0");

    println("WALLETSPAM CODE_HASH={}", code.hash());
    println("WALLETSPAM POOL_PUBKEY={}", pubkey);
    println("WALLETSPAM MINTER={}", minter);
    println("WALLETSPAM WALLET0={}", wallet0Addr);
    println(
        "WALLETSPAM FUNDING wallet0={} repro={} perWallet={}",
        wallet0Funding,
        reproFunding,
        perWallet,
    );

    // 1) deploy wallet 0 — skip funding if it already exists, so the deploy is RETRY-SAFE under the
    //    indexer race that can drop the kickoff after wallet0 was already funded (re-running would otherwise
    //    double-fund it). On a retry wallet0 is up + seqno still 0 (its kickoff never landed), so the kickoff
    //    below re-sends correctly.
    if (scripts.isContractDeployed(wallet0Addr)) {
        println("WALLETSPAM WALLET0 already deployed — skipping funding, re-sending kickoff");
    } else {
        net
            .send(
                deployer.address,
                createMessage({ bounce: BounceMode.NoBounce, value: wallet0Funding, dest: wallet0 }),
            )
            .waitForFirstTransaction();
    }

    if (count <= 0) {
        println("WALLETSPAM POOL_SIZE=1 (count<=0, only wallet 0 deployed)");
        return wallet0Addr;
    }

    // 2) kickoff: wallet0 --ext--> Reproduce to wallet1 (which self-replicates the rest on-chain)
    val extBody = buildKickoffExternal(
        kp,
        0,
        wallet0Addr,
        minter,
        jwCode,
        count,
        chunk,
        deployJetton,
        walletValue,
        jettonValue,
        reproFunding,
    );
    val extResult = net.sendExternal(net.createExternalMessage(wallet0Addr, extBody));
    if (!extResult.isAccepted()) {
        println("WALLETSPAM REPRODUCE_ACCEPTED=false ERROR={}", extResult.error!.message);
        return wallet0Addr;
    }
    extResult.waitForTrace();

    // 3) verify the chain materialized every wallet in [0, count] (and its jetton wallet).
    var deployed = 0;
    var jettons = 0;
    var i = 0;
    while (i <= count) {
        if (scripts.isContractDeployed(walletSpamAddress(i, pubkey, minter, jwCode))) {
            deployed += 1;
        }
        if (deployJetton) {
            val jw = calcAddressOfJettonWallet(
                walletSpamAddress(i, pubkey, minter, jwCode),
                minter,
                jwCode,
            );
            if (scripts.isContractDeployed(jw)) {
                jettons += 1;
            }
        }
        i += 1;
    }
    println("WALLETSPAM POOL_DEPLOYED={}/{} (wallets 0..{})", deployed, count + 1, count);
    if (deployJetton) {
        println("WALLETSPAM JETTONS_DEPLOYED={}/{}", jettons, count + 1);
    }
    return wallet0Addr;
}

// ---- teardown builders + driver (see scripts/destroy.tolk) ----

// Sign a sig-first external: signature(512) || subwallet|validUntil|seqno|(sendMode(8)|^msg)*.
fun finalizePoolExternal(kp: KeyPair, signed: cell): cell {
    val sig = crypto.rawSign(kp.privateKey, signed.hash());
    return beginCell().storeSlice(sig).storeSlice(signed.beginParse()).endCell();
}

// The single kick-off external for the whole teardown, signed on wallet 0. It wraps two messages that
// wallet 0 executes: (a) destroy wallet 0's OWN jetton wallet (forward the WithdrawGrams `withdrawBody`
// to it with a fixed jettonDestroyValue), and (b) hand wallet 0's whole balance to wallet 1 via
// CARRY_ALL_BALANCE|DESTROY, carrying the DestroyChain that drives the linear teardown of the rest.
// count<=0 (only wallet 0) => (b) sweeps straight to `collector` instead. `withdrawBody` is built once
// here and carried down the chain, so no wallet rebuilds it.
fun buildDestroyExternal(
    kp: KeyPair,
    seqno: int,
    minter: address,
    jwCode: cell,
    count: int,
    collector: address,
    jettonDestroyValue: coins,
): cell {
    val pub = kp.publicKey;
    val wallet0Addr = walletSpamAddress(0, pub, minter, jwCode);
    val withdrawBody = WithdrawGrams {
        queryId: 0,
        to: collector,
    }.toCell();

    // (a) wallet 0 destroys its own jetton wallet — same action every wallet performs in the chain.
    val w0JettonDestroy = createMessage({
        bounce: BounceMode.NoBounce,
        value: jettonDestroyValue,
        dest: calcAddressOfJettonWallet(wallet0Addr, minter, jwCode),
        body: withdrawBody,
    }).messageCell;

    var b = beginCell()
        .storeUint(0, 32)
        .storeUint(VALID_UNTIL, 32)
        .storeUint(seqno, 32)
        .storeUint(SEND_MODE_PAY_FEES_SEPARATELY, 8)
        .storeRef(w0JettonDestroy);

    // (b) roll wallet 0's balance onward + self-destruct: to wallet 1 (drives the chain) or, if wallet 0
    //     is the whole pool, straight to the collector.
    if (count <= 0) {
        val w0Sweep = createMessage({
            bounce: BounceMode.NoBounce,
            value: 0,
            dest: collector,
        }).messageCell;
        b = b.storeUint(SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_DESTROY, 8).storeRef(w0Sweep);
    } else {
        val destroyChainMsg = createMessage({
            bounce: BounceMode.NoBounce,
            value: 0,
            dest: walletSpamAddress(1, pub, minter, jwCode),
            body: DestroyChain {
                lastSubwallet: count,
                collector,
                jettonDestroyValue,
                withdrawBody,
            },
        }).messageCell;
        b = b
            .storeUint(SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_DESTROY, 8)
            .storeRef(destroyChainMsg);
    }

    return finalizePoolExternal(kp, b.endCell());
}

// Tear the whole pool down from ONE signed external on wallet 0 (like deploy): wallet 0 destroys its own
// jetton wallet and rolls its balance to wallet 1, which runs the linear DestroyChain — every wallet
// destroys its own jetton wallet and hands its whole balance to the next, so all GRAM ends up at
// `collector`. jettonDestroyValue is the fixed GRAM each wallet forwards to its jetton wallet.
fun destroyWalletSpamPool(
    count: int,
    minter: address,
    jwCode: cell,
    collector: address,
    jettonDestroyValue: coins,
): void {
    val kp = loadPoolKeyPair();
    val pubkey = kp.publicKey;

    println("DESTROY COLLECTOR={}", collector);
    println("DESTROY MINTER={}", minter);
    println("DESTROY COUNT={}", count);

    val wallet0Addr = walletSpamAddress(0, pubkey, minter, jwCode);
    if (!scripts.isContractDeployed(wallet0Addr)) {
        println("DESTROY SKIPPED wallet0 not deployed (wrong minter/count?)");
        return;
    }
    val w0Seqno = WalletSpam.fromAddress(wallet0Addr).seqno();
    val destroyExt = buildDestroyExternal(
        kp,
        w0Seqno,
        minter,
        jwCode,
        count,
        collector,
        jettonDestroyValue,
    );
    val res = net.sendExternal(net.createExternalMessage(wallet0Addr, destroyExt));
    if (!res.isAccepted()) {
        println("DESTROY NOT_ACCEPTED err={}", res.error!.message);
        return;
    }
    res.waitForTrace();

    // ---- verify teardown ----
    var wsAlive = 0;
    var jwAlive = 0;
    var j = 0;
    while (j <= count) {
        val wsAddr = walletSpamAddress(j, pubkey, minter, jwCode);
        if (scripts.isContractDeployed(wsAddr)) {
            wsAlive += 1;
        }
        if (scripts.isContractDeployed(calcAddressOfJettonWallet(wsAddr, minter, jwCode))) {
            jwAlive += 1;
        }
        j += 1;
    }
    println("DESTROY DONE ws_alive={} jw_alive={} (both should be 0)", wsAlive, jwAlive);
}
