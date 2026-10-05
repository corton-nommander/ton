import "@acton/build"
import "@acton/crypto"
import "@acton/emulation/network"
import "@acton/emulation/testing"
import "@acton/testing/expect"

import "@contracts/WalletSpam"
import "@contracts/jetton-utils"
import "@contracts/messages"
import "@wrappers/JettonMinter.gen"
import "@wrappers/JettonWallet.gen"
import "@wrappers/WalletSpam.gen"
import "@wrappers/utils"

import "test-utils"

// Small pool with a deliberately tiny chunk so a modest N still exercises the multi-hop chain:
// N=5, chunk=2  =>  wallet1 drives {2 leaf, 3 driver}; wallet3 drives {4 leaf, 5 driver/terminal}.
// So two real hops (wallet1 -> wallet3 -> wallet5) with only 5 wallets to emulate.
const POOL_N = 5
const POOL_CHUNK = 2
const WALLET_VALUE = grams("2") // grams each pool wallet keeps
const JETTON_VALUE = grams("0.01") // grams each jetton wallet is born with (min balance ~ storage rent)
const WALLET0_FUND = grams("60") // total funded into wallet 0
const REPRO_FUNDING = grams("45") // grams handed to wallet 1 to drive the chain
const JETTON_DESTROY_VALUE = grams("0.05") // fixed grams each wallet forwards to its jetton wallet on teardown
const FAR_FUTURE = 0xFFFFFFFF

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------

// reproduce(deployJetton=true): the whole pool 0..N comes up as one chain, and every pool wallet
// (including wallet 0, deployed via the kickoff wrapper) has a prepaid jetton wallet.
get fun `test reproduce: chain deploys every WalletSpam and its prepaid jetton wallet`() {
    val (minter, deployer, _) = setupTest();
    val kp = crypto.generateKeyPair();
    val jwCode = build("JettonWallet");

    val wallet0 = deployWallet0(kp, minter.address, jwCode, deployer.address);

    val ext = buildKickoffExternal(
        kp,
        0,
        wallet0.address,
        minter.address,
        jwCode,
        true,
    );
    wallet0.sendAnyExternal(ext).unwrap();

    // every pool wallet exists, and so does its prepaid jetton wallet (born with PREPAID_BALANCE)
    var i = 0;
    while (i <= POOL_N) {
        val ws = poolAddr(i, kp, minter.address, jwCode);
        expect(testing.isDeployed(ws)).toEqual(true);

        val jw = poolJettonAddr(i, kp, minter.address, jwCode);
        expect(testing.isDeployed(jw)).toEqual(true);
        expect(JettonWallet { address: jw }.getJettonBalance()).toEqual(PREPAID_BALANCE);
        i += 1;
    }

    // seqno bumped exactly once on the entry wallet
    expect(wallet0.seqno()).toEqual(1);
}

// reproduce(deployJetton=false): a "top-up" pass still stands up the pool wallets in order, but
// deploys NO jetton wallets.
get fun `test reproduce: deployJetton=false stands up pool without jetton wallets`() {
    val (minter, deployer, _) = setupTest();
    val kp = crypto.generateKeyPair();
    val jwCode = build("JettonWallet");

    val wallet0 = deployWallet0(kp, minter.address, jwCode, deployer.address);

    val ext = buildKickoffExternal(
        kp,
        0,
        wallet0.address,
        minter.address,
        jwCode,
        false,
    );
    wallet0.sendAnyExternal(ext).unwrap();

    var i = 0;
    while (i <= POOL_N) {
        expect(testing.isDeployed(poolAddr(i, kp, minter.address, jwCode))).toEqual(true);
        // no jetton wallet materialized
        expect(testing.isDeployed(poolJettonAddr(i, kp, minter.address, jwCode))).toEqual(false);
        i += 1;
    }
}

// destroy: one signed external on wallet 0 drives the LINEAR teardown — every pool wallet destroys its
// own jetton wallet AND self-destructs, rolling all GRAM to `collector`. Both the pool and every jetton
// wallet vanish, and the collector picks up the balances.
get fun `test destroy: linear chain wipes the whole pool AND its jetton wallets into collector`() {
    val (minter, deployer, _) = setupTest();
    val kp = crypto.generateKeyPair();
    val jwCode = build("JettonWallet");
    val collector = testing.treasury("collector");

    val wallet0 = deployWallet0(kp, minter.address, jwCode, deployer.address);
    wallet0
        .sendAnyExternal(
            buildKickoffExternal(
                kp,
                0,
                wallet0.address,
                minter.address,
                jwCode,
                true,
            ),
        )
        .unwrap();

    val collectorBefore = testing.getAccountBalance(collector.address);

    // seqno is now 1 (deploy kickoff used seqno 0)
    val destroyExt = buildDestroyExternal(kp, 1, minter.address, jwCode, collector.address);
    wallet0.sendAnyExternal(destroyExt).unwrap();

    // whole pool AND every jetton wallet is gone
    var wsAlive = 0;
    var jwAlive = 0;
    var i = 0;
    while (i <= POOL_N) {
        if (testing.isDeployed(poolAddr(i, kp, minter.address, jwCode))) {
            wsAlive += 1;
        }
        if (testing.isDeployed(poolJettonAddr(i, kp, minter.address, jwCode))) {
            jwAlive += 1;
        }
        i += 1;
    }
    expect(wsAlive).toEqual(0); // no pool wallet survives
    expect(jwAlive).toEqual(0); // no jetton wallet survives

    // collector picked up the rolled-up + swept balances
    expect(testing.getAccountBalance(collector.address) > collectorBefore).toEqual(true);
}

// external guards: valid external is accepted; wrong seqno / subwallet / signature are all rejected
// before acceptance.
get fun `test external: seqno, subwallet, signature guards`() {
    val (minter, deployer, _) = setupTest();
    val kp = crypto.generateKeyPair();
    val jwCode = build("JettonWallet");
    val wallet0 = deployWallet0(kp, minter.address, jwCode, deployer.address);

    // a bare, correctly-signed external (no wrapped messages) is accepted and bumps seqno
    expect(wallet0.sendAnyExternal(bareExternal(kp, 0, 0, FAR_FUTURE))).toBeAccepted();
    expect(wallet0.seqno()).toEqual(1);

    // wrong seqno (stale)
    expect(wallet0.sendAnyExternal(bareExternal(kp, 0, 0, FAR_FUTURE))).toBeNotAccepted();
    // wrong subwallet id
    expect(wallet0.sendAnyExternal(bareExternal(kp, 9, 1, FAR_FUTURE))).toBeNotAccepted();
    // already-expired
    expect(wallet0.sendAnyExternal(bareExternal(kp, 0, 1, 1))).toBeNotAccepted();
    // bad signature (signed with a different key)
    val wrongKp = crypto.generateKeyPair();
    expect(
        wallet0.sendAnyExternal(bareExternalSignedBy(wrongKp, 0, 1, FAR_FUTURE)),
    ).toBeNotAccepted();

    // a real external still works afterwards (state wasn't corrupted by the rejects)
    expect(wallet0.sendAnyExternal(bareExternal(kp, 0, 1, FAR_FUTURE))).toBeAccepted();
    expect(wallet0.seqno()).toEqual(2);
}

get fun `test signed internal: relayer pays for a GRAM transfer and shares external seqno`() {
    val (minter, deployer, _) = setupTest();
    val kp = crypto.generateKeyPair();
    val jwCode = build("JettonWallet");
    val wallet0 = deployWallet0(kp, minter.address, jwCode, deployer.address);
    val relayer = testing.treasury("gasless-relayer");
    val receiver = testing.treasury("gasless-receiver");
    val transferValue = grams("0.001");

    val body = signedInternalTransfer(
        kp,
        0,
        0,
        FAR_FUTURE,
        receiver.address,
        transferValue,
    );
    val res = wallet0.sendAny(relayer.address, body, { value: grams("0.1"), bounce: true });

    expect(res).toHaveTx({
        from: wallet0.address,
        to: receiver.address,
        value: transferValue,
    });
    expect(wallet0.seqno()).toEqual(1);

    // The gasless request consumed the same counter as the legacy signed-external channel.
    expect(wallet0.sendAnyExternal(bareExternal(kp, 0, 0, FAR_FUTURE))).toBeNotAccepted();
    expect(wallet0.sendAnyExternal(bareExternal(kp, 0, 1, FAR_FUTURE))).toBeAccepted();
    expect(wallet0.seqno()).toEqual(2);
}

get fun `test signed internal: pool wallet relays a target-signed request end to end`() {
    val (minter, deployer, _) = setupTest();
    val kp = crypto.generateKeyPair();
    val jwCode = build("JettonWallet");
    val wallet0 = deployWallet0(kp, minter.address, jwCode, deployer.address);
    val wallet1 = WalletSpam.fromStorage({
        seqno: 0,
        subwalletId: 1,
        publicKey: kp.publicKey,
        minterAddress: minter.address,
        jettonWalletCode: jwCode,
    });
    expect(
        wallet1.deploy(deployer.address, { value: grams("2"), bounce: false }),
    ).toHaveSuccessfulDeploy({ to: wallet1.address });
    val receiver = testing.treasury("relay-e2e-receiver");
    val transferValue = grams("0.001");
    val targetBody = signedInternalTransfer(
        kp,
        1,
        0,
        FAR_FUTURE,
        receiver.address,
        transferValue,
    );
    val relayMessage = createMessage({
        bounce: true,
        value: grams("0.01"),
        dest: wallet1.address,
        body: targetBody,
    }).messageCell;
    val outer = signedExternalWithMessage(
        kp,
        0,
        0,
        FAR_FUTURE,
        SEND_MODE_PAY_FEES_SEPARATELY | SEND_MODE_IGNORE_ERRORS,
        relayMessage,
    );

    val res = wallet0.sendAnyExternal(outer).unwrap();
    expect(res).toHaveTx({ from: wallet0.address, to: wallet1.address });
    expect(res).toHaveTx({
        from: wallet1.address,
        to: receiver.address,
        value: transferValue,
    });
    expect(wallet0.seqno()).toEqual(1);
    expect(wallet1.seqno()).toEqual(1);
}

get fun `test signed internal: invalid signature is a silent deposit`() {
    val (minter, deployer, _) = setupTest();
    val kp = crypto.generateKeyPair();
    val wrongKp = crypto.generateKeyPair();
    val jwCode = build("JettonWallet");
    val wallet0 = deployWallet0(kp, minter.address, jwCode, deployer.address);
    val relayer = testing.treasury("bad-signature-relayer");
    val receiver = testing.treasury("bad-signature-receiver");
    val transferValue = grams("0.001");

    val body = signedInternalTransfer(
        wrongKp,
        0,
        0,
        FAR_FUTURE,
        receiver.address,
        transferValue,
    );
    val res = wallet0.sendAny(relayer.address, body, { value: grams("0.1"), bounce: true });

    expect(res).toHaveSuccessfulTx({ from: relayer.address, to: wallet0.address, exitCode: 0 });
    expect(res).toNotHaveTx({ from: wallet0.address, to: receiver.address });
    expect(wallet0.seqno()).toEqual(0);
}

get fun `test signed internal: send actions must ignore action errors`() {
    val (minter, deployer, _) = setupTest();
    val kp = crypto.generateKeyPair();
    val jwCode = build("JettonWallet");
    val wallet0 = deployWallet0(kp, minter.address, jwCode, deployer.address);
    val relayer = testing.treasury("bad-mode-relayer");
    val receiver = testing.treasury("bad-mode-receiver");
    val body = signedInternalTransfer(
        kp,
        0,
        0,
        FAR_FUTURE,
        receiver.address,
        grams("0.001"),
        SEND_MODE_PAY_FEES_SEPARATELY,
    );

    val res = wallet0.sendAny(relayer.address, body, { value: grams("0.1"), bounce: true });
    expect(res).toHaveTx({ from: relayer.address, to: wallet0.address, exitCode: 402 });
    expect(res).toNotHaveTx({ from: wallet0.address, to: receiver.address });
    expect(wallet0.seqno()).toEqual(0);
}

// internal auth: a reproduce that does not come from the claimed parent driver is rejected (401).
get fun `test reproduce: unauthorized sender rejected`() {
    val (minter, deployer, notDeployer) = setupTest();
    val kp = crypto.generateKeyPair();
    val jwCode = build("JettonWallet");
    val wallet0 = deployWallet0(kp, minter.address, jwCode, deployer.address);
    wallet0
        .sendAnyExternal(
            buildKickoffExternal(
                kp,
                0,
                wallet0.address,
                minter.address,
                jwCode,
                false,
            ),
        )
        .unwrap();

    val wallet1 = WalletSpam.fromAddress(poolAddr(1, kp, minter.address, jwCode));
    val res = wallet1.sendReproduce(
        notDeployer.address, // not wallet 0 -> must fail the sibling check
        POOL_N,
        0,
        POOL_CHUNK,
        false,
        WALLET_VALUE,
        JETTON_VALUE,
        { value: grams("1"), bounce: true },
    );
    expect(res).toHaveFailedTx<Reproduce>({
        from: notDeployer.address,
        to: wallet1.address,
        exitCode: 401,
    });
}

// chunk clamp: an out-of-range chunk aborts the first driver (ERR_BAD_CHUNK) instead of silently
// overrunning the 255-action limit, so the chain never propagates past wallet 1.
get fun `test reproduce: over-limit chunk is rejected (chain does not propagate)`() {
    val (minter, deployer, _) = setupTest();
    val kp = crypto.generateKeyPair();
    val jwCode = build("JettonWallet");
    val wallet0 = deployWallet0(kp, minter.address, jwCode, deployer.address);

    // chunk = MAX_CHUNK+1 (254) — over the on-chain clamp; wallet 1's Reproduce must revert.
    val ext = buildKickoffExternal(
        kp,
        0,
        wallet0.address,
        minter.address,
        jwCode,
        false,
        254,
    );
    val res = wallet0.sendAnyExternal(ext).unwrap();

    // wallet 1's driver hop failed with ERR_BAD_CHUNK (38)...
    expect(res).toHaveFailedTx<Reproduce>({
        to: poolAddr(1, kp, minter.address, jwCode),
        exitCode: 38,
    });
    // ...so nothing below wallet 1 was deployed.
    expect(testing.isDeployed(poolAddr(2, kp, minter.address, jwCode))).toEqual(false);
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

// state_init that deploys pool wallet #subwallet (all siblings share the exact WalletSpam code).
fun poolStateInit(
    subwallet: int,
    publicKey: uint256,
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
                publicKey,
                minterAddress: minter,
                jettonWalletCode: jwCode,
            }.toCell(),
        },
    };
}

fun poolAddr(subwallet: int, kp: KeyPair, minter: address, jwCode: cell): address {
    return poolStateInit(subwallet, kp.publicKey, minter, jwCode).calculateAddress();
}

fun poolJettonAddr(subwallet: int, kp: KeyPair, minter: address, jwCode: cell): address {
    return calcAddressOfJettonWallet(poolAddr(subwallet, kp, minter, jwCode), minter, jwCode);
}

fun deployWallet0(kp: KeyPair, minter: address, jwCode: cell, from: address): WalletSpam {
    val wallet0 = WalletSpam.fromStorage({
        seqno: 0,
        subwalletId: 0,
        publicKey: kp.publicKey,
        minterAddress: minter,
        jettonWalletCode: jwCode,
    });
    val res = wallet0.deploy(from, { value: WALLET0_FUND, bounce: false });
    expect(res).toHaveSuccessfulDeploy({ to: wallet0.address });
    return wallet0;
}

// Wrap a signed external around a (signature | subwallet | validUntil | seqno) header. The signed
// portion (everything after the 512-bit signature, data + refs) is exactly what gets hashed.
fun finalizeExternal(kp: KeyPair, signed: cell): cell {
    val sig = crypto.rawSign(kp.privateKey, signed.hash());
    return beginCell().storeSlice(sig).storeSlice(signed.beginParse()).endCell();
}

// A signed external carrying no wrapped messages — just exercises the header guards.
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

fun signedInternalTransfer(
    kp: KeyPair,
    subwallet: int,
    seqno: int,
    validUntil: int,
    receiver: address,
    value: coins,
    sendMode: int = SEND_MODE_PAY_FEES_SEPARATELY | SEND_MODE_IGNORE_ERRORS,
): cell {
    val outMessage = createMessage({ bounce: false, value, dest: receiver }).messageCell;
    val actions = OutActionWithSendMessageOnly {
        prev: createEmptyCell() as Cell<OutActionWithSendMessageOnlyOrEmpty>,
        sendMode: sendMode as uint8,
        outMsg: outMessage,
    }.toCell();
    val payload = InternalSignedRequest {
        walletId: subwallet,
        validUntil,
        seqno,
        outActions: actions,
        hasExtraActions: false,
        extraActions: createEmptyCell().beginParse(),
    }.toCell();
    val signature = crypto.rawSign(kp.privateKey, payload.hash());
    return beginCell().storeSlice(payload.beginParse()).storeSlice(signature).endCell();
}

fun signedExternalWithMessage(
    kp: KeyPair,
    subwallet: int,
    seqno: int,
    validUntil: int,
    sendMode: int,
    message: cell,
): cell {
    val signed = beginCell()
        .storeUint(subwallet, 32)
        .storeUint(validUntil, 32)
        .storeUint(seqno, 32)
        .storeUint(sendMode, 8)
        .storeRef(message)
        .endCell();
    return finalizeExternal(kp, signed);
}

// Kickoff external: wraps wallet 0's own jetton deploy (when deployJetton) plus the reproduce message
// that deploys wallet 1 and starts the chain.
fun buildKickoffExternal(
    kp: KeyPair,
    seqno: int,
    wallet0Addr: address,
    minter: address,
    jwCode: cell,
    deployJetton: bool,
    chunk: int = POOL_CHUNK,
): cell {
    val reproMsg = createMessage({
        bounce: BounceMode.Only256BitsOfBody,
        value: REPRO_FUNDING,
        dest: poolStateInit(1, kp.publicKey, minter, jwCode),
        body: Reproduce {
            lastSubwallet: POOL_N,
            fromSubwallet: 0,
            chunk,
            deployJetton,
            walletValue: WALLET_VALUE,
            jettonValue: JETTON_VALUE,
        },
    }).messageCell;

    var b = beginCell()
        .storeUint(0, 32) // subwallet 0
        .storeUint(FAR_FUTURE, 32)
        .storeUint(seqno, 32);

    if (deployJetton) {
        val w0Jetton = createMessage({
            bounce: BounceMode.NoBounce,
            value: JETTON_VALUE,
            dest: calcDeployedJettonWallet(wallet0Addr, minter, jwCode),
            body: TopUpGrams {},
        }).messageCell;
        b = b.storeUint(SEND_MODE_PAY_FEES_SEPARATELY, 8).storeRef(w0Jetton);
    }
    b = b.storeUint(SEND_MODE_REGULAR, 8).storeRef(reproMsg);

    return finalizeExternal(kp, b.endCell());
}

// Teardown external: wallet 0 destroys its own jetton wallet (forwarding a WithdrawGrams body carried
// down the chain) + rolls its balance to wallet 1 via CARRY_ALL_BALANCE|DESTROY, kicking off the linear
// DestroyChain. Mirrors buildDestroyExternal in scripts/utils/walletspam.tolk.
fun buildDestroyExternal(
    kp: KeyPair,
    seqno: int,
    minter: address,
    jwCode: cell,
    collector: address,
): cell {
    val withdrawBody = WithdrawGrams {
        queryId: 0,
        to: collector,
    }.toCell();

    val w0JettonDestroy = createMessage({
        bounce: BounceMode.NoBounce,
        value: JETTON_DESTROY_VALUE,
        dest: poolJettonAddr(0, kp, minter, jwCode),
        body: withdrawBody,
    }).messageCell;

    val destroyChainMsg = createMessage({
        bounce: BounceMode.NoBounce,
        value: 0,
        dest: poolAddr(1, kp, minter, jwCode),
        body: DestroyChain {
            lastSubwallet: POOL_N,
            collector,
            jettonDestroyValue: JETTON_DESTROY_VALUE,
            withdrawBody,
        },
    }).messageCell;

    val signed = beginCell()
        .storeUint(0, 32)
        .storeUint(FAR_FUTURE, 32)
        .storeUint(seqno, 32)
        .storeUint(SEND_MODE_PAY_FEES_SEPARATELY, 8)
        .storeRef(w0JettonDestroy)
        .storeUint(SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_DESTROY, 8)
        .storeRef(destroyChainMsg)
        .endCell();

    return finalizeExternal(kp, signed);
}
