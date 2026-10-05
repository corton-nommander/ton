// Deploy parity-vector generator: for a FIXED (seed, minter, pool params), print the exact kickoff
// external cell hash produced by buildKickoffExternal, plus wallet0/wallet1/jetton addresses. The Go
// native deploy (go-spam/cmd/deploy) must reproduce this hash byte-for-byte
// (go-spam/pkg/deploy/parity_test.go). Re-run `acton script scripts/_deploy_parity.tolk` and update the
// Go wantHash if WalletSpam/JettonWallet code, the Reproduce struct, or the kickoff layout changes.
import "@acton/build"
import "@acton/crypto"
import "@acton/io"
import "@acton/emulation/network"
import "@contracts/jetton-utils"
import "utils/walletspam"

fun main() {
    val seed = 0x0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210;
    val kp = crypto.seedToKeyPair(seed);
    val minter = address("0:0000000000000000000000000000000000000000000000000000000000000abc");
    val jwCode = build("JettonWallet");

    val count = 5;
    val chunk = 2;
    val deployJetton = true;
    val walletValue: coins = 200000000;
    val jettonValue: coins = 10000000;
    val feeSlack: coins = 10000000;
    val perWallet = walletValue + feeSlack;
    val wallet0Funding = (count + 1) * perWallet;
    val reproFunding = count * perWallet;

    val wallet0Addr = walletSpamAddress(0, kp.publicKey, minter, jwCode);
    val wallet1Addr = walletSpamAddress(1, kp.publicKey, minter, jwCode);
    val w0Jetton = calcAddressOfJettonWallet(wallet0Addr, minter, jwCode);

    val ext = buildKickoffExternal(
        kp, 0, wallet0Addr, minter, jwCode,
        count, chunk, deployJetton, walletValue, jettonValue, reproFunding,
    );

    val (w0wc, w0hash) = wallet0Addr.getWorkchainAndHash();
    val (w1wc, w1hash) = wallet1Addr.getWorkchainAndHash();
    val (jwwc, jwhash) = w0Jetton.getWorkchainAndHash();

    println("DEPLOYPARITY PUBKEY={}", kp.publicKey);
    println("DEPLOYPARITY WALLET0 wc={} hash={}", w0wc, w0hash);
    println("DEPLOYPARITY WALLET1 wc={} hash={}", w1wc, w1hash);
    println("DEPLOYPARITY W0JETTON wc={} hash={}", jwwc, jwhash);
    println("DEPLOYPARITY WALLET0_FUNDING={}", wallet0Funding);
    println("DEPLOYPARITY REPRO_FUNDING={}", reproFunding);
    println("DEPLOYPARITY KICKOFF_HASH={}", ext.hash());
}
