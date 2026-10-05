// Transfer-cost probe (emulation): the FAITHFUL bot flow — a WalletSpam external emits op::transfer to
// its own (prepaid) jetton wallet, which forwards op::internal_transfer to the recipient, which returns
// op::excesses. Sums totalFees across the whole trace, for three variants. (Delete after; tool.)
// Run: acton script scripts/_transfer_cost.tolk
import "@acton/build"
import "@acton/crypto"
import "@acton/io"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "@acton/types/big_array"
import "@acton/types/transaction"
import "@contracts/jetton-utils"
import "@contracts/messages"
import "@contracts/storage"
import "@wrappers/JettonMinter.gen"
import "utils/walletspam"

const TRANSFER_VALUE = 160000000 // 0.16 GRAM attached to op::transfer (matches the bot)
const PAY_SEP = 1 // SEND_MODE_PAY_FEES_SEPARATELY (matches the bot's external sendMode)

fun tinyForwardPayload(): slice {
    return beginCell().storeUint(0, 1).toSlice();
}

fun initModePayload(mode: int): cell {
    return beginCell().storeUint(mode, 2).endCell();
}

fun reportTrace(label: string, res: SendResultList) {
    var total: coins = 0;
    var i = 0;
    while (i < res.size()) {
        val fee = res.at(i).tx.load().totalFees.grams;
        total += fee;
        println("  {} tx[{}] fee_nano={}", label, i, fee);
        i += 1;
    }
    println("COST {} txs={} total_fee_nano={}", label, res.size(), total);
}

fun buildTransfer(recipient: address, excessesTo: address, custom: cell?): AskToTransfer {
    return AskToTransfer {
        queryId: 0,
        jettonAmount: 1,
        transferRecipient: recipient,
        sendExcessesTo: excessesTo,
        customPayload: custom,
        forwardTonAmount: 0,
        forwardPayload: tinyForwardPayload(),
    };
}

// sign an external on a WalletSpam that wraps a single internal message with the given send mode
fun signExternal(kp: KeyPair, subwallet: int, seqno: int, sendMode: int, msg: cell): cell {
    val signed = beginCell()
        .storeUint(subwallet, 32)
        .storeUint(0xFFFFFFFF, 32)
        .storeUint(seqno, 32)
        .storeUint(sendMode, 8)
        .storeRef(msg)
        .endCell();
    val sig = crypto.rawSign(kp.privateKey, signed.hash());
    return beginCell().storeSlice(sig).storeSlice(signed.beginParse()).endCell();
}

fun main() {
    val deployer = scripts.wallet("deployer");
    val jwCode = build("JettonWallet");
    val kp = crypto.generateKeyPair();

    val minter = JettonMinter.fromStorage({
        totalSupply: 0,
        adminAddress: deployer.address,
        nextAdminAddress: null,
        metadata: beginCell().storeUint(1, 8).storeString("x").endCell(),
    });
    minter.deploy(deployer.address, { value: grams("0.05") }).waitForFirstTransaction();

    // Two pool wallets (same key): #0's jetton wallet is pre-deployed (sender for mode 0 / mode 1);
    // #1's jetton wallet is left undeployed (to measure init on op::transfer).
    val ws0 = walletSpamStateInit(0, kp.publicKey, minter.address, jwCode);
    val ws1 = walletSpamStateInit(1, kp.publicKey, minter.address, jwCode);
    val ws0Addr = ws0.calculateAddress();
    val ws1Addr = ws1.calculateAddress();
    net
        .send(deployer.address, createMessage({ bounce: false, value: grams("3"), dest: ws0 }))
        .waitForFirstTransaction();
    net
        .send(deployer.address, createMessage({ bounce: false, value: grams("3"), dest: ws1 }))
        .waitForFirstTransaction();
    // pre-deploy ws0's own jetton wallet (the sender), prepaid
    net
        .send(
            deployer.address,
            createMessage({
                bounce: false,
                value: grams("0.2"),
                dest: calcDeployedJettonWallet(ws0Addr, minter.address, jwCode),
                body: TopUpGrams {},
            }),
        )
        .waitForFirstTransaction();
    val ws0Jw = calcAddressOfJettonWallet(ws0Addr, minter.address, jwCode);

    val recipA = address("0:1111111111111111111111111111111111111111111111111111111111111111");
    val recipB = address("0:2222222222222222222222222222222222222222222222222222222222222222");
    val recipC = address("0:3333333333333333333333333333333333333333333333333333333333333333");

    // A) MODE 0: op::transfer NO init (ws0 jw pre-deployed) + op::internal_transfer WITH init (recipient fresh)
    val opTransferA = createMessage({
        bounce: true,
        value: TRANSFER_VALUE,
        dest: ws0Jw, // plain — sender jw already deployed
        body: buildTransfer(recipA, ws0Addr, null),
    }).messageCell;
    val resA = net
        .sendExternal(
            net.createExternalMessage(ws0Addr, signExternal(kp, 0, 0, PAY_SEP, opTransferA)),
        )
        .waitForTrace()!;
    reportTrace("MODE0", resA);

    // B) MODE 0 + init on op::transfer: ws1's jetton wallet is ALSO deployed in the op::transfer
    val opTransferB = createMessage({
        bounce: true,
        value: TRANSFER_VALUE,
        dest: calcDeployedJettonWallet(
            ws1Addr,
            minter.address,
            jwCode,
        ),                                                               // AutoDeployAddress => sender jw deployed here
        body: buildTransfer(recipB, ws1Addr, null),
    }).messageCell;
    val resB = net
        .sendExternal(
            net.createExternalMessage(ws1Addr, signExternal(kp, 1, 0, PAY_SEP, opTransferB)),
        )
        .waitForTrace()!;
    reportTrace("MODE0_SENDERINIT", resB);

    // C) MODE 1: op::transfer no init + op::internal_transfer no init (recipient pre-deployed)
    net
        .send(
            deployer.address,
            createMessage({
                bounce: false,
                value: grams("0.2"),
                dest: calcDeployedJettonWallet(recipC, minter.address, jwCode),
                body: TopUpGrams {},
            }),
        )
        .waitForFirstTransaction();
    val opTransferC = createMessage({
        bounce: true,
        value: TRANSFER_VALUE,
        dest: ws0Jw,
        body: buildTransfer(recipC, ws0Addr, initModePayload(1)),
    }).messageCell;
    val resC = net
        .sendExternal(
            net.createExternalMessage(ws0Addr, signExternal(kp, 0, 1, PAY_SEP, opTransferC)),
        )
        .waitForTrace()!;
    reportTrace("MODE1", resC);

    println("DONE");
}
