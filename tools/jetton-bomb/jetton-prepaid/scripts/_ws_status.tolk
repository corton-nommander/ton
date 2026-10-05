// WalletSpam pool status probe (SAMPLED): for ~WS_SAMPLES subwallets spread across [0, WALLETSPAM_COUNT]
// (incl. 0 and the terminal), report how many WalletSpam wallets AND their prepaid jetton wallets are
// deployed. Env: POOL_KEY_FILE, JETTON_MINTER_ADDRESS, WALLETSPAM_COUNT, WS_SAMPLES.
// Run: POOL_KEY_FILE=pool-test-1k.key JETTON_MINTER_ADDRESS=kQ... WALLETSPAM_COUNT=1000 \
//      acton script scripts/_ws_status.tolk --net testnet
import "@acton/build"
import "@acton/env"
import "@acton/io"
import "@acton/prompts"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "@contracts/WalletSpam"
import "@contracts/jetton-utils"
import "@wrappers/WalletSpam.gen"
import "utils/common"
import "utils/walletspam"

fun main() {
    val count = env<int>("WALLETSPAM_COUNT") ?? 1000;
    val SAMPLES = env<int>("WS_SAMPLES") ?? 12;
    val minter = promptJettonAddress("JETTON_MINTER_ADDRESS", "Minter address");
    val pubkey = loadPoolKeyPair().publicKey;
    val jwCode = build("JettonWallet");

    val step = count <= SAMPLES ? 1 : count / (SAMPLES - 1);
    var wsDep = 0;
    var jwDep = 0;
    var checked = 0;
    var i = 0;
    while (i <= count) {
        val wsAddr = walletSpamAddress(i, pubkey, minter, jwCode);
        checked += 1;
        if (scripts.isContractDeployed(wsAddr)) {
            wsDep += 1;
        }
        if (scripts.isContractDeployed(calcAddressOfJettonWallet(wsAddr, minter, jwCode))) {
            jwDep += 1;
        }
        if (i == count) {
            i = count + 1;
        } else if (i + step > count) {
            i = count;
        } else {
            i += step;
        }
    }
    val termWs = scripts.isContractDeployed(walletSpamAddress(count, pubkey, minter, jwCode));
    println("WS SAMPLE checked={} ws_deployed={} jw_deployed={} terminal_ws={}", checked, wsDep, jwDep, termWs);
}
