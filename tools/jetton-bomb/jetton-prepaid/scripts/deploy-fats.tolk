// One-shot deploy of a FatReproduce pool: N self-replicating accounts, each of which builds a large,
// per-account-UNIQUE, incompressible cell chain in-chain — inflating the network's basechain state by
// ~count * chainCells * 120 bytes (logical; ~1.7x on disk). For inflating a controlled testnet's state
// toward mainnet size WITHOUT a genesis restart (no zerostate/hardfork needed).
//
// Emulate:   acton script scripts/deploy-fats.tolk
// Broadcast: acton script scripts/deploy-fats.tolk --net <network>
// Env (non-interactive): FAT_DEPLOYER, FAT_COUNT, FAT_CHAIN_CELLS, FAT_BUILD_STEP, FAT_CHUNK,
//   FAT_WALLET_VALUE, FAT_BUILD_VALUE, FAT_FEE_SLACK.
//
// Sizing: chainCells ~= targetBytesPerAccount / 120. 100 KB ≈ 833 cells; 32 KB ≈ 273 cells.
// buildStep <= 180 on stock config (measured ~5400 gas/cell vs the 1M basechain gas_limit). On a network
// where you hold the config key, raise ConfigParam-21 gas_limit/block_gas_limit (and make the deployer a
// ConfigParam-31 special account) and buildStep can be much larger — fewer FatBuild txs per account.
//
// Pool keypair: raw 32-byte ed25519 seed in jetton-prepaid/fat-pool.key, auto-generated + saved on first
// run. It controls the whole pool (redeploy/destroy) — back it up.

import "@acton/build"
import "@acton/env"
import "@acton/io"
import "@acton/prompts"
import "@acton/emulation/scripts"
import "utils/common"
import "utils/fatreproduce"

fun main() {
    val deployer = promptJettonWallet("FAT_DEPLOYER", "Select deployer wallet");

    val count = env<int>("FAT_COUNT") ?? promptInt("Fat pool size (count, accounts after wallet 0)", "10");
    val chainCells = env<int>("FAT_CHAIN_CELLS") ??
    promptInt("Cells each account builds (~120 bytes each; 833 ≈ 100 KB)", "833");
    val buildStep = env<int>("FAT_BUILD_STEP") ??
    promptInt("Cells built per FatBuild tx (<=180 on stock gas config)", "180");
    // Children per driver hop — fixed at 250 (contract clamps to MAX_CHUNK=253; leaves headroom under the
    // 255-action cap for the reserve + self-build).
    val chunk = 250;

    // Funding. walletValue must cover the whole per-account build: ceil(chainCells/buildStep) FatBuild txs
    // at ~0.065 GRAM gas each (testnet 66.67 nanoTON/gas * ~968k gas) plus a resting balance. Defaults are
    // generous (grams are free on the target testnet). buildValue funds one FatBuild tx.
    val walletValue = promptJettonCoins("FAT_WALLET_VALUE", "GRAM each account keeps + self-builds from", "1500000000"); // 1.5
    val buildValue = promptJettonCoins("FAT_BUILD_VALUE", "GRAM per FatBuild self-message", "250000000"); // 0.25
    val feeSlack = promptJettonCoins("FAT_FEE_SLACK", "GRAM fee slack per account", "500000000"); // 0.5

    deployFatPool(deployer, count, walletValue, buildValue, feeSlack, chunk, chainCells, buildStep);

    println("DEPLOY DONE");
}
