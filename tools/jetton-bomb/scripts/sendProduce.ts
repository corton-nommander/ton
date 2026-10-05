import { compile, NetworkProvider } from '@ton/blueprint';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';
import { openWalletR } from './lib/wallets';

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    ui.write(`pubkey: ${formatPublicKey(keypair)}`);

    const code = await compile('WalletR');
    const wallet = await openWalletR(provider, keypair, 0, code);

    const count = Number(await ui.input('Enter the subwallets count:'));

    await wallet.sendReproduce(count);
}
