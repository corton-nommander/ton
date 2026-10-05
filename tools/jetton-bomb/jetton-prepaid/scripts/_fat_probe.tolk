// Probe a spread of individual subwallets of a fat pool: print address + deployed + cellsBuilt per wallet
// (balance is fetched separately via toncenter, it's not a get-method). Env: FAT_KEY_FILE, FAT_COUNT,
// FAT_SAMPLES. Run: FAT_KEY_FILE=fat-salt-5g2.key FAT_COUNT=29000 acton script scripts/_fat_probe.tolk --net testnet
import "@acton/build"
import "@acton/env"
import "@acton/io"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "@contracts/FatReproduce"
import "@wrappers/FatReproduce.gen"
import "utils/fatreproduce"

fun main() {
    val count = env<int>("FAT_COUNT") ?? 29000;
    val SAMPLES = env<int>("FAT_SAMPLES") ?? 8;
    val pk = loadFatPoolKeyPair().publicKey;
    val step = count <= SAMPLES ? 1 : count / (SAMPLES - 1);
    var i = 0;
    while (i <= count) {
        val addr = fatAddress(i, pk);
        var dep = false;
        var cells = -1;
        if (scripts.isContractDeployed(addr)) {
            dep = true;
            cells = FatReproduce.fromAddress(addr).getCellsBuilt();
        }
        println("PROBE sw={} deployed={} cells={} addr={}", i, dep, cells, addr);
        if (i == count) {
            i = count + 1;
        } else if (i + step > count) {
            i = count;
        } else {
            i += step;
        }
    }
}
