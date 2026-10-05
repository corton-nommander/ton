// Fixed WalletSpam `sint` + outer relay vectors for the Go builders.
import "@acton/build"
import "@acton/crypto"
import "@acton/io"
import "@contracts/messages"
import "utils/walletspam"

fun main() {
    val seed = 0x0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210;
    val kp = crypto.seedToKeyPair(seed);
    val destination = address(
        "0:0000000000000000000000000000000000000000000000000000000000000def",
    );
    val outMessage = createMessage({
        bounce: false,
        value: 1234567,
        dest: destination,
    }).messageCell;
    val actions = OutActionWithSendMessageOnly {
        prev: createEmptyCell() as Cell<OutActionWithSendMessageOnlyOrEmpty>,
        sendMode: (SEND_MODE_PAY_FEES_SEPARATELY | SEND_MODE_IGNORE_ERRORS) as uint8,
        outMsg: outMessage,
    }.toCell();
    val payload = InternalSignedRequest {
        walletId: 7,
        validUntil: 0xFFFFFFFF,
        seqno: 9,
        outActions: actions,
        hasExtraActions: false,
        extraActions: createEmptyCell().beginParse(),
    }.toCell();
    val signature = crypto.rawSign(kp.privateKey, payload.hash());
    val body = beginCell().storeSlice(payload.beginParse()).storeSlice(signature).endCell();

    val minter = address(
        "0:0000000000000000000000000000000000000000000000000000000000000abc",
    );
    val target = walletSpamAddress(7, kp.publicKey, minter, build("JettonWallet"));
    val relayMessage = createMessage({
        bounce: true,
        value: 10000000,
        dest: target,
        body,
    }).messageCell;
    val outerPayload = beginCell()
        .storeUint(0, 32)
        .storeUint(0xFFFFFFFF, 32)
        .storeUint(3, 32)
        .storeUint(SEND_MODE_PAY_FEES_SEPARATELY | SEND_MODE_IGNORE_ERRORS, 8)
        .storeRef(relayMessage)
        .endCell();
    val outerSignature = crypto.rawSign(kp.privateKey, outerPayload.hash());
    val outer = beginCell()
        .storeSlice(outerSignature)
        .storeSlice(outerPayload.beginParse())
        .endCell();

    println("GASLESSPARITY ACTIONS_HASH={}", actions.hash());
    println("GASLESSPARITY PAYLOAD_HASH={}", payload.hash());
    println("GASLESSPARITY BODY_HASH={}", body.hash());
    println("GASLESSPARITY OUTER_HASH={}", outer.hash());
}
