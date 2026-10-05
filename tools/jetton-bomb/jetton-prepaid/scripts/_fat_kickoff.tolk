// Kickoff-ONLY resend for a fat pool whose wallet0 is already funded + deployed but whose kickoff external
// raced ahead of wallet0 activation (net "destination account has no active state" — happens under load).
// Re-sends ONLY the signed kickoff external; does NOT re-fund wallet0. Env must match the original deploy's
// funding params: FAT_KEY_FILE, FAT_COUNT, FAT_CHAIN_CELLS, FAT_BUILD_STEP, FAT_WALLET_VALUE, FAT_BUILD_VALUE,
// FAT_FEE_SLACK. Broadcast: acton script scripts/_fat_kickoff.tolk --net testnet
import "@acton/build"
import "@acton/env"
import "@acton/io"
import "@acton/prompts"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "@contracts/FatReproduce"
import "@wrappers/FatReproduce.gen"
import "utils/common"
import "utils/fatreproduce"

fun main() {
    val kp = loadFatPoolKeyPair();
    val pubkey = kp.publicKey;

    val count = env<int>("FAT_COUNT") ?? promptInt("Fat pool size (count)", "29000");
    val chainCells = env<int>("FAT_CHAIN_CELLS") ?? promptInt("Cells each account builds", "833");
    val buildStep = env<int>("FAT_BUILD_STEP") ?? promptInt("Cells per FatBuild tx", "180");
    val walletValue = promptJettonCoins("FAT_WALLET_VALUE", "GRAM each account keeps + self-builds from", "1000000000");
    val buildValue = promptJettonCoins("FAT_BUILD_VALUE", "GRAM per FatBuild self-message", "250000000");
    val feeSlack = promptJettonCoins("FAT_FEE_SLACK", "GRAM fee slack per account", "200000000");
    val chunk = 250;

    val code = build("FatReproduce");
    val wallet0Addr = fatAddress(0, pubkey);
    net.registerCodeCell(code, "FatReproduce");
    net.registerAddress(wallet0Addr, "FatReproduce#0");

    if (!scripts.isContractDeployed(wallet0Addr)) {
        println("WALLET0 NOT DEPLOYED {} — fund it first (deploy-fats)", wallet0Addr);
        return;
    }
    val seqno = FatReproduce.fromAddress(wallet0Addr).seqno();
    if (seqno != 0) {
        // Kickoff already fired (seqno advanced). Re-sending would double-spend the reproduce funding —
        // skip. Makes this script safe to always run after a deploy as a race-recovery no-op.
        println("KICKOFF SKIPPED already-fired seqno={} wallet0={}", seqno, wallet0Addr);
        return;
    }
    val perWallet = walletValue + feeSlack;
    val reproFunding = count * perWallet;
    println("KICKOFF-ONLY wallet0={} seqno={} count={} reproFunding={}", wallet0Addr, seqno, count, reproFunding);

    val ext = buildFatKickoffExternal(
        kp, seqno, wallet0Addr, count, chunk, walletValue, chainCells, buildStep, buildValue, reproFunding,
    );
    val res = net.sendExternal(net.createExternalMessage(wallet0Addr, ext));
    if (!res.isAccepted()) {
        println("KICKOFF NOT_ACCEPTED err={}", res.error!.message);
        return;
    }
    res.waitForTrace();
    println("KICKOFF RESENT OK count={}", count);
}
