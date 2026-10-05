// Dump every fat-account address of one pool to stdout as `ADDR <address> <sizeKB>` lines (redirect to a
// file). Address = pure state_init hash (subwalletId + pubkey + code), independent of the built size — so
// one run per pool/key. Runs LOCALLY (no --net, no RPC): the address is computed, not fetched.
// Run: FAT_KEY_FILE=fat-200k.key FAT_COUNT=4999 FAT_SIZE_KB=200 acton script scripts/dump-fat-addrs.tolk
import "@acton/build"
import "@acton/env"
import "@acton/io"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "@contracts/FatReproduce"
import "@wrappers/FatReproduce.gen"
import "utils/fatreproduce"

fun main() {
    val count = env<int>("FAT_COUNT") ?? 0;
    val sizeKb = env<int>("FAT_SIZE_KB") ?? 0;
    val pk = loadFatPoolKeyPair().publicKey;
    var i = 0;
    while (i <= count) {
        println("ADDR {} {}", fatAddress(i, pk), sizeKb);
        i += 1;
    }
    println("DONE {} addrs size={}KB", count + 1, sizeKb);
}
