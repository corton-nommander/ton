// Parity-vector generator: fixed (pubkey, minter, jwCode) -> WalletSpam#0 + its jetton wallet
// addresses. The Go side asserts these exact hashes (go-spam pkg/walletspam/parity_test.go and
// cmd/bot/parity_test.go). If WalletSpam/JettonWallet storage or code changes, re-run
// `acton script scripts/_parity.tolk` and update the wantHash constants in those Go tests.
import "@acton/build"
import "@acton/io"
import "@acton/emulation/network"
import "@contracts/jetton-utils"
import "utils/walletspam"

fun main() {
    val pubkey = 0x0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210;
    val minter = address("0:0000000000000000000000000000000000000000000000000000000000000abc");
    val jwCode = build("JettonWallet");

    val w0 = walletSpamAddress(0, pubkey, minter, jwCode);
    val (w0wc, w0hash) = w0.getWorkchainAndHash();
    println("PARITY WALLETSPAM0 wc={} hash={}", w0wc, w0hash);

    val jw = calcAddressOfJettonWallet(w0, minter, jwCode);
    val (jwwc, jwhash) = jw.getWorkchainAndHash();
    println("PARITY JETTON0 wc={} hash={}", jwwc, jwhash);
}
