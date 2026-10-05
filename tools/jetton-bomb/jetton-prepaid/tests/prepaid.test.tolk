import "@acton/build"
import "@acton/emulation/network"
import "@acton/emulation/testing"
import "@acton/testing/expect"
import "@acton/types/transaction"

import "@contracts/errors"
import "@contracts/jetton-utils"
import "@contracts/messages"
import "@contracts/storage"
import "@wrappers/JettonMinter.gen"
import "@wrappers/JettonWallet.gen"
import "@wrappers/utils"

import "test-utils"

// A freshly deployed wallet is "born" with PREPAID_BALANCE jettons, with no mint.
get fun `test prepaid: freshly deployed wallet is born with PREPAID_BALANCE`() {
    val (minter, deployer, _) = setupTest();
    val wallet = deployWalletFromStorage(
        WalletStorage {
            jettonBalance: 0,
            ownerAddress: deployer.address,
            minterAddress: minter.address,
        },
        deployer.address,
    );

    expect(wallet.getJettonBalance()).toEqual(PREPAID_BALANCE);
}

// The core load path: an owner can transfer without any prior mint; both the sender
// and the (auto-deployed) recipient reflect the prepaid balance.
get fun `test prepaid: owner transfers with no mint, both sides prepaid`() {
    val (minter, deployer, notDeployer) = setupTest();
    val senderWallet = deployWalletFromStorage(
        WalletStorage {
            jettonBalance: 0,
            ownerAddress: deployer.address,
            minterAddress: minter.address,
        },
        deployer.address,
    );
    val receiverWallet = userWallet(minter, notDeployer.address); // not deployed yet
    val sent = 400000;                                            // < PREPAID_BALANCE (1_000_000)

    val res = senderWallet.sendTransfer(
        deployer.address,
        DEFAULT_TRANSFER_MSG_VALUE,
        sent,
        notDeployer.address,
        deployer.address,
        null,
        0, // no forward notification
        null,
    );
    expect(res).toHaveSuccessfulTx<AskToTransfer>({
        from: deployer.address,
        to: senderWallet.address,
    });

    expect(senderWallet.getJettonBalance()).toEqual(PREPAID_BALANCE - sent);
    expect(receiverWallet.getJettonBalance()).toEqual(PREPAID_BALANCE + sent);
}

// Plain (not sharded): the on-chain wallet address equals a pure state_init hash.
get fun `test plain: wallet address is a pure state_init hash`() {
    val (minter, _, _) = setupTest();
    val owner = testing.treasury("plain-owner").address;

    val fromContract = minter.getWalletAddress(owner);
    val fromUtil = calcAddressOfJettonWallet(owner, minter.address, build("JettonWallet"));

    expect(fromContract).toEqual(fromUtil);
}

// Teardown: owner sweeps the whole GRAM balance to a custom address and the wallet
// self-destructs.
get fun `test withdraw: owner sweeps all GRAM to custom address, wallet self-destructs`() {
    val (minter, deployer, notDeployer) = setupTest();
    val wallet = deployWalletFromStorage(
        WalletStorage {
            jettonBalance: 0,
            ownerAddress: deployer.address,
            minterAddress: minter.address,
        },
        deployer.address,
        grams("5"), // fund the wallet with GRAM to reclaim later
    );
    expect(testing.isDeployed(wallet.address)).toEqual(true);

    val customDest = notDeployer.address;
    val destBefore = testing.getAccountBalance(customDest);

    val res = wallet.sendWithdrawGrams(
        deployer.address,
        0,
        customDest,
        { bounce: false, value: grams("0.05") },
    );
    expect(res).toHaveSuccessfulTx<WithdrawGrams>({
        from: deployer.address,
        to: wallet.address,
    });

    // wallet is gone, and the swept GRAM landed on the custom address
    expect(testing.isDeployed(wallet.address)).toEqual(false);
    expect(testing.getAccountBalance(customDest) > destBefore).toEqual(true);
}

// Only the owner may withdraw/destroy.
get fun `test withdraw: non-owner cannot withdraw`() {
    val (minter, deployer, notDeployer) = setupTest();
    val wallet = deployWalletFromStorage(
        WalletStorage {
            jettonBalance: 0,
            ownerAddress: deployer.address,
            minterAddress: minter.address,
        },
        deployer.address,
        grams("2"),
    );

    val res = wallet.sendWithdrawGrams(
        notDeployer.address,
        0,
        null,
        { bounce: false, value: grams("0.05") },
    );
    expect(res).toHaveFailedTx<WithdrawGrams>({
        from: notDeployer.address,
        to: wallet.address,
        exitCode: Errors.NotOwner,
    });
    expect(testing.isDeployed(wallet.address)).toEqual(true);
}
