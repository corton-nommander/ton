// Fat pool status probe (SAMPLED, for big pools): read cellsBuilt for ~40 wallets spread across
// [0, FAT_COUNT] (always incl. 0 and the terminal `count`, the causally-last wallet in the reproduce
// spine) and report the deployed/built fraction of the sample. Checking all 29k wallets would be hours of
// RPC. Run: FAT_COUNT=28900 FAT_CHAIN_CELLS=833 acton script scripts/_fat_status.tolk --net testnet
import "@acton/build"
import "@acton/env"
import "@acton/io"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "@contracts/FatReproduce"
import "@wrappers/FatReproduce.gen"
import "utils/fatreproduce"

fun main() {
    val count = env<int>("FAT_COUNT") ?? 100;
    val target = env<int>("FAT_CHAIN_CELLS") ?? 833;
    val SAMPLES = env<int>("FAT_SAMPLES") ?? 40;
    val pk = loadFatPoolKeyPair().publicKey;

    val step = count <= SAMPLES ? 1 : count / (SAMPLES - 1);
    var deployed = 0;
    var built = 0;
    var checked = 0;
    var minCells = -1;
    var maxCells = 0;
    var i = 0;
    while (i <= count) {
        val addr = fatAddress(i, pk);
        checked += 1;
        if (scripts.isContractDeployed(addr)) {
            deployed += 1;
            val c = FatReproduce.fromAddress(addr).getCellsBuilt();
            if (c >= target) {
                built += 1;
            }
            if (minCells < 0 || c < minCells) {
                minCells = c;
            }
            if (c > maxCells) {
                maxCells = c;
            }
        }
        // advance: sample by `step`, but always finish exactly on `count` (the terminal)
        if (i == count) {
            i = count + 1;
        } else if (i + step > count) {
            i = count;
        } else {
            i += step;
        }
    }
    val termUp = scripts.isContractDeployed(fatAddress(count, pk));
    println("FAT SAMPLE checked={} deployed={} built(>={})={}", checked, deployed, target, built);
    println("FAT SAMPLE minCells={} maxCells={} terminal_deployed={}", minCells, maxCells, termUp);
}
