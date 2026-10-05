// Tear down a FatReproduce pool (see contracts/FatReproduce.tolk): ONE signed external on wallet 0 drives
// the linear FatDestroy roll-up — every account self-destructs (dropping its big chain from state) and
// rolls its whole balance to `collector`. Uses jetton-prepaid/fat-pool.key (must be the pool's key).
//
// Emulate:   acton script scripts/destroy-fats.tolk
// Broadcast: acton script scripts/destroy-fats.tolk --net testnet
// Env: FAT_DEPLOYER (its address = collector for the swept GRAM), FAT_COUNT (must match the deploy).

import "@acton/build"
import "@acton/env"
import "@acton/io"
import "@acton/prompts"
import "@acton/emulation/scripts"
import "utils/common"
import "utils/fatreproduce"

fun main() {
    val collectorWallet = promptJettonWallet("FAT_DEPLOYER", "Select collector wallet (gets swept GRAM)");
    val count = env<int>("FAT_COUNT") ?? promptInt("Fat pool size (count) that was deployed", "10");
    destroyFatPool(count, collectorWallet.address);
    println("DESTROY DONE");
}
