import "sharding"
import "storage"

// Load-testing hack: every jetton wallet is "born" pre-funded with this balance,
// so any wallet can transfer immediately without a real mint. Total supply is NOT
// conserved — intentional, for spam/load simulation only.
const PREPAID_BALANCE = 1000000

fun calcDeployedJettonWallet(
    ownerAddress: address,
    minterAddress: address,
    jettonWalletCode: cell,
): AutoDeployAddress {
    val emptyWalletStorage: WalletStorage = {
        jettonBalance: PREPAID_BALANCE,
        ownerAddress,
        minterAddress,
    };

    // Plain jetton (not sharded): address is a pure state_init hash, no shard pinning.
    return {
        workchain: MY_WORKCHAIN,
        stateInit: { code: jettonWalletCode, data: emptyWalletStorage.toCell() },
    };
}

fun calcAddressOfJettonWallet(
    ownerAddress: address,
    minterAddress: address,
    jettonWalletCode: cell,
): address {
    val jwDeployed = calcDeployedJettonWallet(ownerAddress, minterAddress, jettonWalletCode);
    return jwDeployed.calculateAddress();
}
