// genkey: make sure the pool keypair exists. On first run it generates + SAVES jetton-prepaid/pool.key
// (the raw 32-byte ed25519 seed the acton deploy and the Go bot share) and prints its pubkey; if pool.key
// already exists it just prints the existing pubkey (idempotent). No network, no prompts.
//
// Run: acton script scripts/genkey.tolk
import "@acton/io"
import "utils/walletspam"

fun main() {
    val kp = loadPoolKeyPair();
    println("POOL_PUBKEY={}", kp.publicKey);
    println("GENKEY DONE — pool.key is jetton-prepaid/pool.key (32-byte ed25519 seed). Back it up.");
}
