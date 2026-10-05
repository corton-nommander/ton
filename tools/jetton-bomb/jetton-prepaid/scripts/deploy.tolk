// One-shot deploy: prepaid jetton minter + the WalletSpam spam pool.
//
// Ported from scripts/deployWalletSpam.ts so the whole load-test rig comes up from a single Tolk
// script. Everything (minter, jetton wallets, WalletSpam) is native Tolk in this package, compiled via
// build(...) — no prebuilt BoC (see utils/walletspam.tolk).
//
// Emulate:   acton script scripts/deploy.tolk
// Broadcast: acton script scripts/deploy.tolk --net testnet
// Env overrides (non-interactive): JETTON_DEPLOYER, JETTON_ADMIN_ADDRESS, JETTON_URI, WALLETSPAM_COUNT,
//   WALLET_VALUE, JETTON_VALUE, FEE_SLACK, DEPLOY_CHUNK, DEPLOY_JETTON.
// Pool keypair: a raw 32-byte ed25519 seed in jetton-prepaid/pool.key, auto-generated + SAVED on first
// run (never a lost throwaway). The Go bot reads the SAME file, so pool addresses match — back it up, it
// controls the whole pool.

import "@acton/build"
import "@acton/env"
import "@acton/io"
import "@acton/prompts"
//import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "@contracts/storage"
import "@wrappers/JettonMinter.gen"
import "utils/common"
import "utils/walletspam"

// Same metadata URI as scripts/deployWalletSpam.ts.
const DEFAULT_JETTON_URI = "https://gist.githubusercontent.com/1IxI1/356016d859d140ee092327b4b429ba54/raw/pub5.json"

// Off-chain content (TEP-64): 0x01 prefix + snake URI. Mirrors wrappers/JettonMinter.ts
// jettonContentToCell({ type: 1, uri }).
fun buildOffchainContent(uri: string): cell {
    return beginCell().storeUint(1, 8).storeString(uri).endCell();
}

fun main() {
    val deployer = promptJettonWallet("JETTON_DEPLOYER", "Select deployer wallet");
    val admin = promptJettonAddress(
        "JETTON_ADMIN_ADDRESS",
        "Enter admin address",
        deployer.address,
    );
    val uri = promptJettonString("JETTON_URI", "Enter jetton metadata URI", DEFAULT_JETTON_URI);

    // 1) Prepaid plain jetton minter (off-chain URI metadata).
    val storage: MinterStorage = {
        totalSupply: 0,
        adminAddress: admin,
        nextAdminAddress: null,
        metadata: buildOffchainContent(uri),
    };
    val minter = deployJettonMinter(deployer, storage);
    val data = minter.getJettonData();
    println("JETTON MINTER_ADDRESS={}", minter.address);
    println("JETTON TOTAL_SUPPLY={}", data.totalSupply);
    println("JETTON ADMIN_ADDRESS={}", data.adminAddress);
    printJettonWalletInfo(minter, admin, "JETTON_ADMIN");

    // 2) WalletSpam pool [0, count]. count<=0 deploys only wallet 0.
    val count = env<int>("WALLETSPAM_COUNT") ?? promptInt("WalletSpam pool size (count)", "0");
    // Single funding lever: each wallet keeps walletValue and (when deployJetton) its jetton wallet
    // gets jettonValue. chunk = children per driver hop (1..253). deployJetton off => top-up-only pass.
    val walletValue = promptJettonCoins(
        "WALLET_VALUE",
        "GRAM kept per pool wallet (nano)",
        "200000000",
    );
    val jettonValue = promptJettonCoins(
        "JETTON_VALUE",
        "GRAM per jetton-wallet deploy (nano)",
        "10000000", // 0.01 GRAM — ~1.3 years of basechain storage rent; enough for a throwaway test jw
    );
    // Fee headroom on top of walletValue (mostly rolls to the tail wallet, recoverable). Default 0.015 GRAM
    // (FEE_SLACK_PER_WALLET) — 1.5x the already-safe 0.01 (a live mainnet 5k pool at 0.01 finished with ~43
    // of 50 GRAM slack unused). Mainnet cascade cost is only ~0.001/wallet; raise FEE_SLACK if a huge pool's
    // terminal wallet ever won't deploy. (Devnet fees are ~20x higher — cmd/deploy uses 0.1 there.)
    val feeSlack = promptJettonCoins(
        "FEE_SLACK",
        "GRAM fee slack per pool wallet (nano)",
        "15000000",
    );
    val chunk = env<int>("DEPLOY_CHUNK") ?? promptInt("Children per driver hop (1..253)", "200");
    val deployJetton = (env<int>("DEPLOY_JETTON") ??
    promptInt("Deploy jetton wallets in-chain? (1/0)", "1")) !=
    0;
    deployWalletSpamPool(
        deployer,
        count,
        minter.address,
        build("JettonWallet"),
        walletValue,
        jettonValue,
        feeSlack,
        chunk,
        deployJetton,
    );

    println("DEPLOY DONE");
}
