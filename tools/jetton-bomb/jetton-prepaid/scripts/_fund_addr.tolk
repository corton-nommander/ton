// Fund an arbitrary address from JETTON_DEPLOYER — used to top up the Go highload-deploy control wallet
// (the funds live in the acton keystore deployer; the Go V5R1 signer is separate).
//
//   FUND_ADDR=EQ... FUND_NANO=340000000000 JETTON_DEPLOYER=mainnet-w5-stress \
//     acton script scripts/_fund_addr.tolk --net testnet
// (FUND_NANO is nanoTON; omit it to send the 340 TON default.)
import "@acton/env"
import "@acton/io"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "utils/common"

fun main() {
    val deployer = promptJettonWallet("JETTON_DEPLOYER", "Select deployer wallet");
    val dest = promptJettonAddress("FUND_ADDR", "Address to fund");
    val amount = env<coins>("FUND_NANO") ?? grams("340");
    println("FUNDING {} with {} nanoTON from {}", dest, amount, deployer.address);
    net
        .send(
            deployer.address,
            createMessage({ bounce: BounceMode.NoBounce, value: amount, dest }),
        )
        .waitForFirstTransaction();
    println("FUND DONE dest={} amount={}", dest, amount);
}
