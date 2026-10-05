// Print WalletSpam + its prepaid jetton-wallet address (friendly form) for a handful of subwallets, LOCALLY
// (no --net) — for handing the user explorer-checkable examples. Env: POOL_KEY_FILE, JETTON_MINTER_ADDRESS.
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

fun printSub(s: int, pubkey: int, minter: address, jwCode: cell) {
    val ws = walletSpamAddress(s, pubkey, minter, jwCode);
    val jw = calcAddressOfJettonWallet(ws, minter, jwCode);
    println("SUB {} ws={} jw={}", s, ws, jw);
}

fun main() {
    val minter = promptJettonAddress("JETTON_MINTER_ADDRESS", "Minter");
    val pubkey = loadPoolKeyPair().publicKey;
    val jwCode = build("JettonWallet");
    printSub(0, pubkey, minter, jwCode);
    printSub(1, pubkey, minter, jwCode);
    printSub(307, pubkey, minter, jwCode);
    printSub(545, pubkey, minter, jwCode);
}
