import { toNano } from '@ton/core';
import { compile, NetworkProvider } from '@ton/blueprint';
import { mnemonicToWalletKey } from '@ton/crypto';
import { ensureMnemonic, formatPublicKey } from './lib/mnemonic';
import { openWalletR } from './lib/wallets';

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const mnemonic = await ensureMnemonic(ui);
    const phrase = mnemonic.join(' ');
    ui.write(`mnemonic: ${phrase}`);
    const keypair = await mnemonicToWalletKey(mnemonic);
    ui.write(`pubkey: ${formatPublicKey(keypair)}`);

    const code = await compile('WalletR');
    const wallet = await openWalletR(provider, keypair, 0, code);
    ui.write('0th wallet: ' + wallet.address.toString());

    const amount = toNano(await ui.input('Enter deploy TON amount:'));
    const currencyAmount = toNano(await ui.input('Enter deploy currency amount:'));

    await wallet.sendDeploy(provider.sender(), amount, currencyAmount);

    await provider.waitForDeploy(wallet.address);
}
