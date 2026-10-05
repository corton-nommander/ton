// Sample pool wallets' seqno. Pool wallets are born (via reproduce) with seqno 0; only a processed external
// bumps it — wallet0 by the deploy kickoff, and any OTHER wallet ONLY by a processed gasless relayer-external
// or sint request. So a non-wallet0 wallet with seqno>0 is decisive proof a gasless op landed on-chain.
// Env: POOL_KEY_FILE, JETTON_MINTER_ADDRESS, WALLETSPAM_COUNT, WS_SAMPLES.
import "@acton/build"
import "@acton/env"
import "@acton/io"
import "@acton/prompts"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "@contracts/WalletSpam"
import "@wrappers/WalletSpam.gen"
import "utils/common"
import "utils/walletspam"

fun main() {
    val count = env<int>("WALLETSPAM_COUNT") ?? 1000;
    val SAMPLES = env<int>("WS_SAMPLES") ?? 20;
    val minter = promptJettonAddress("JETTON_MINTER_ADDRESS", "Minter address");
    val pubkey = loadPoolKeyPair().publicKey;
    val jwCode = build("JettonWallet");

    val step = count <= SAMPLES ? 1 : count / (SAMPLES - 1);
    var nonzero = 0;
    var checked = 0;
    var i = 1; // skip wallet0 (its seqno=1 is from the deploy kickoff, not gasless)
    while (i <= count) {
        val addr = walletSpamAddress(i, pubkey, minter, jwCode);
        if (scripts.isContractDeployed(addr)) {
            checked += 1;
            val sq = WalletSpam.fromAddress(addr).seqno();
            if (sq > 0) {
                nonzero += 1;
                println("  wallet subwallet={} seqno={} -> gasless op processed", i, sq);
            }
        }
        i += step;
    }
    println("WS SEQNO checked={} nonzero_nonwallet0={}", checked, nonzero);
}
