import { toNano } from '@ton/core';
import { compile, NetworkProvider } from '@ton/blueprint';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';
import { openWalletSpam } from './lib/wallets';

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    ui.write(`pubkey: ${formatPublicKey(keypair)}`);

    const code = await compile('WalletSpam');
    const wallet = await openWalletSpam(provider, keypair, 0, { code });
    ui.write(`0th wallet: ${wallet.address.toString()}`);

    const count = Number(await ui.input('Enter the last subwallet no:'));
    const ton_amount = toNano(await ui.input('Enter the ton amount:'));
    let currencyAmount: bigint | undefined = toNano(await ui.input('Enter the currency amount:'));
    if (currencyAmount == 0n) currencyAmount = undefined;

    await wallet.sendReproduce(count, ton_amount, undefined, currencyAmount);
}
