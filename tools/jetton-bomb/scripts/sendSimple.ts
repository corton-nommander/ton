import { SendMode, beginCell, toNano } from '@ton/core';
import { compile, NetworkProvider } from '@ton/blueprint';
import { WalletSpam, spamMsg } from '../wrappers/WalletSpam';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    ui.write(`Pubkey: ${formatPublicKey(keypair)}`);

    const wallet = provider.open(
        WalletSpam.createFromConfig(
            { subwallet_id: 20001, keypair: keypair, spam_msg: spamMsg },
            await compile('WalletSpam'),
        ),
    );
    ui.write(`WalletSpam address: ${wallet.address.toString()}`);
    await ui.input('Press Enter to continue');

    await wallet.sendMsgAsWallet(
        beginCell()
            .storeUint(0x10, 6)
            .storeAddress(provider.sender().address)
            .storeCoins(toNano('79000'))
            .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)
            .storeUint(0xd53276db, 32) // excesses
            .storeUint(0, 64)
            .endCell(),
        SendMode.NONE,
    );
    ui.write(`Seqno: ${await wallet.getSeqno()}`);
    // await wallet.send({
    //     to: provider.sender().address!,
    //     value: 1n,
    //     sendMode: SendMode.PAY_GAS_SEPARATELY,
    // });
}
