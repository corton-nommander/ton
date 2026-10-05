// One-shot teardown: reclaim GRAM from the whole WalletSpam pool and destroy it. The counterpart to
// scripts/deploy.tolk. Everything is signed by the pool keypair (jetton-prepaid/pool.key) — no funder needed.
//
// One signed external on wallet 0 (like deploy) drives the whole LINEAR teardown (see
// destroyWalletSpamPool in utils/walletspam.tolk): every pool wallet destroys its OWN jetton wallet
// (sweeping its GRAM to your address) and hands its whole balance to the next wallet, so all GRAM rolls
// down the chain to your address and the entire pool self-destructs. No per-wallet externals anymore.
//
// Emulate:   acton script scripts/destroy.tolk
// Broadcast: acton script scripts/destroy.tolk --net testnet
// Env (non-interactive): DESTROY_COLLECTOR, JETTON_MINTER, WALLETSPAM_COUNT, JETTON_DESTROY_VALUE.
//
// NOTE: use the SAME minter address and pool size (count) the pool was deployed with — the pool address
// is derived from (pool pubkey, minter, JettonWallet code), so a wrong minter points at the wrong pool.

import "@acton/build"
import "@acton/env"
import "@acton/io"
import "@acton/prompts"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "utils/common"
import "utils/walletspam"

fun main() {
    // Where reclaimed GRAM goes (jetton sweeps in phase 1 + subwallet roll-up in phase 2). Your wallet.
    val collector = promptJettonAddress(
        "DESTROY_COLLECTOR",
        "Enter your wallet (reclaim GRAM here)",
    );
    // Minter the pool was deployed against (part of every pool + jetton address).
    val minter = promptJettonAddress("JETTON_MINTER", "Enter the pool's jetton minter address");
    val count = env<int>("WALLETSPAM_COUNT") ?? promptInt("WalletSpam pool size (count)", "0");

    // Fixed GRAM forwarded to each wallet's jetton wallet to fund its self-destruct. It reaches the
    // collector via the jetton path, so keep it small enough that the main roll-up chain never runs dry.
    val jettonDestroyValue = promptJettonCoins(
        "JETTON_DESTROY_VALUE",
        "GRAM sent to each jetton wallet for its self-destruct (nano)",
        "10000000",
    );

    destroyWalletSpamPool(count, minter, build("JettonWallet"), collector, jettonDestroyValue);

    println("DESTROY SCRIPT DONE");
}
