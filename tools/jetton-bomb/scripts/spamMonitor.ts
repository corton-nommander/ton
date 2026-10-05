import { beginCell, toNano } from '@ton/core';
import { compile, NetworkProvider } from '@ton/blueprint';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';
import { openWalletSpam } from './lib/wallets';
import { monitorDeployment } from './lib/monitor';

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    const keypairSafe = keypair;
    ui.write(`Pubkey: ${formatPublicKey(keypairSafe)}`);

    const spamMsg = beginCell()
        .storeUint(0x10, 6)
        .storeAddress(provider.sender().address)
        .storeCoins(toNano('0.01'))
        .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)
        .storeUint(0xd53276db, 32) // excesses
        .storeUint(0, 64)
        .endCell();

    const code = await compile('WalletSpam');
    async function getRWalletSeqno(subwallet: number) {
        try {
            const wallet = await openWalletSpam(provider, keypairSafe, subwallet, { code, spamMsg });
            return await wallet.getSeqno();
        } catch (e) {
            return -1;
        }
    }

    const target = Number(await ui.input('Enter the total count:'))!;
    await monitorDeployment(ui, target, getRWalletSeqno);
}
