import "@acton/build"
import "@acton/emulation/network"
import "@acton/emulation/testing"
import "@acton/testing/expect"

import "@contracts/jetton-utils"
import "@contracts/messages"
import "@contracts/storage"
import "@wrappers/JettonMinter.gen"
import "@wrappers/JettonWallet.gen"
import "@wrappers/utils"

import "test-utils"

// internal_init_mode carried in customPayload: mode 1 (bare) omits the recipient state_init.
fun initModePayload(mode: int): cell {
    return beginCell().storeUint(mode, 2).endCell();
}

// mode 1 (bare, no state_init) transfers correctly when the recipient is ALREADY deployed.
get fun `test init-mode 1: bare transfer to a pre-deployed recipient works`() {
    val (minter, deployer, notDeployer) = setupTest();
    val sender = deployWalletFromStorage(
        WalletStorage {
            jettonBalance: 0,
            ownerAddress: deployer.address,
            minterAddress: minter.address,
        },
        deployer.address,
    );
    // recipient must already exist for a bare (no-state_init) transfer
    val receiver = deployWalletFromStorage(
        WalletStorage {
            jettonBalance: 0,
            ownerAddress: notDeployer.address,
            minterAddress: minter.address,
        },
        notDeployer.address,
    );
    val sent = 300000;

    val res = sender.sendTransfer(
        deployer.address,
        DEFAULT_TRANSFER_MSG_VALUE,
        sent,
        notDeployer.address,
        deployer.address,
        initModePayload(1), // customPayload = internal_init_mode 1 (bare)
        0,
        null,
    );
    expect(res).toHaveSuccessfulTx<AskToTransfer>({ from: deployer.address, to: sender.address });

    // the (pre-deployed) recipient received the jettons — proving the bare InternalTransferStep landed
    expect(sender.getJettonBalance()).toEqual(PREPAID_BALANCE - sent);
    expect(receiver.getJettonBalance()).toEqual(PREPAID_BALANCE + sent);
}

// mode 0 (nil customPayload) is unchanged classic behavior: deploys the recipient in the same transfer.
get fun `test init-mode 0: full state_init still auto-deploys a fresh recipient`() {
    val (minter, deployer, notDeployer) = setupTest();
    val sender = deployWalletFromStorage(
        WalletStorage {
            jettonBalance: 0,
            ownerAddress: deployer.address,
            minterAddress: minter.address,
        },
        deployer.address,
    );
    val receiver = userWallet(minter, notDeployer.address); // NOT deployed yet
    val sent = 250000;

    val res = sender.sendTransfer(
        deployer.address,
        DEFAULT_TRANSFER_MSG_VALUE,
        sent,
        notDeployer.address,
        deployer.address,
        null, // mode 0 (default)
        0,
        null,
    );
    expect(res).toHaveSuccessfulTx<AskToTransfer>({ from: deployer.address, to: sender.address });

    expect(sender.getJettonBalance()).toEqual(PREPAID_BALANCE - sent);
    expect(receiver.getJettonBalance()).toEqual(PREPAID_BALANCE + sent);
}

// The whole point of mode 1: a bare transfer message is cheaper to forward than a full-state_init one,
// because it omits the wallet code (~855B) + data from the outgoing InternalTransferStep.
get fun `test init-mode: bare forward fee is lower than full state_init fee`() {
    val (minter, _, _) = setupTest();
    val recipient = testing.treasury("fee-recipient").address;
    val jwCode = build("JettonWallet");
    val body = InternalTransferStep {
        queryId: 0,
        jettonAmount: 1,
        transferInitiator: null,
        sendExcessesTo: null,
        forwardTonAmount: 0,
        forwardPayload: makeTinyForwardPayload(),
    };

    val fullMsg = createMessage({
        bounce: BounceMode.Only256BitsOfBody,
        dest: calcDeployedJettonWallet(
            recipient,
            minter.address,
            jwCode,
        ), // AutoDeployAddress => state_init
        value: grams("0.1"),
        body,
    });
    val bareMsg = createMessage({
        bounce: BounceMode.Only256BitsOfBody,
        dest: calcAddressOfJettonWallet(
            recipient,
            minter.address,
            jwCode,
        ), // plain address => no state_init
        value: grams("0.1"),
        body,
    });

    val fullFee = fullMsg.estimateFeeWithoutSending(SEND_MODE_REGULAR);
    val bareFee = bareMsg.estimateFeeWithoutSending(SEND_MODE_REGULAR);
    expect(bareFee < fullFee).toEqual(true);
}
