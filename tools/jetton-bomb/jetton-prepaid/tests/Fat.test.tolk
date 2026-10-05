import "@acton/build"
import "@acton/emulation/network"
import "@acton/emulation/testing"
import "@acton/testing/expect"
import "@acton/types/transaction"

import "@contracts/Fat"
import "@contracts/messages"
import "@wrappers/Fat.gen"

import "test-utils"

fun deployFat(): (Fat, Treasury) {
    val deployer = testing.treasury("fat-deployer");
    val fat = Fat.fromStorage({
        nonce: 0,
        bigDict: beginCell().storeUint(0xFA7, 12).endCell(),
    });
    val res = fat.deploy(deployer.address, { bounce: false, value: grams("1") });
    expect(res).toHaveSuccessfulDeploy({ to: fat.address });
    return (fat, deployer);
}

// A flagged notification (forwardPayload in ref form => leading Either bit 1) bumps the nonce, so the
// storage hash changes and the next collation re-walks the fat's dict.
get fun `test fat: flagged notification bumps nonce`() {
    val (fat, deployer) = deployFat();
    expect(fat.getNonce()).toEqual(0);

    val res = fat.sendTransferNotificationForRecipient(
        deployer.address,
        0,                                             // queryId
        1,                                             // jettonAmount
        deployer.address,                              // transferInitiator
        PayloadInRef { value: beginCell().endCell() as Cell<RemainingBitsAndRefs> }, // ref form => leading bit 1 => mutate
        { bounce: false, value: grams("0.05") },
    );
    expect(res).toHaveSuccessfulTx<TransferNotificationForRecipient>({
        from: deployer.address,
        to: fat.address,
    });
    expect(fat.getNonce()).toEqual(1);
}

// An unflagged notification (forwardPayload inline empty => leading bit 0) still touches the fat but
// leaves its storage unchanged — the A/B "touch without mutating" case.
get fun `test fat: unflagged notification leaves nonce`() {
    val (fat, deployer) = deployFat();
    expect(fat.getNonce()).toEqual(0);

    val res = fat.sendTransferNotificationForRecipient(
        deployer.address,
        0,
        1,
        deployer.address,
        PayloadInline { value: beginCell().toSlice() }, // inline empty => leading bit 0 => no mutate
        { bounce: false, value: grams("0.05") },
    );
    expect(res).toHaveSuccessfulTx<TransferNotificationForRecipient>({
        from: deployer.address,
        to: fat.address,
    });
    expect(fat.getNonce()).toEqual(0);
}

// A non-notification message is silently accepted (no throw, no mutation) so a fat never bounces load.
get fun `test fat: stray message is ignored`() {
    val (fat, deployer) = deployFat();
    val res = fat.sendAny(
        deployer.address,
        beginCell().storeUint(0xdeadbeef, 32).endCell(),
        { bounce: false, value: grams("0.05") },
    );
    expect(res).toHaveSuccessfulTx({ from: deployer.address, to: fat.address });
    expect(fat.getNonce()).toEqual(0);
}
