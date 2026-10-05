import { Address, beginCell, Cell, toNano } from '@ton/core';
import { NetworkProvider } from '@ton/blueprint';
import { WalletSpam } from '../wrappers/WalletSpam';
import { loadKeypairFromEnv, formatPublicKey } from './lib/mnemonic';

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const to = Address.parse(await ui.input('Enter to address:'));
    const amount = toNano(await ui.input('Enter ton amount:'));
    const currencyAmount = toNano(await ui.input('Enter currency amount:'));

    const keypair = await loadKeypairFromEnv(ui);
    if (!keypair) return;
    ui.write(`pubkey: ${formatPublicKey(keypair)}`);

    const config = {
        subwallet_id: 698983191,
        keypair: keypair,
        spam_msg: Cell.EMPTY,
    };

    const addr = provider.sender().address!;
    const wallet = provider.open(WalletSpam.createFromAddress(addr, config));

    await wallet.send({
        to: to,
        value: amount,
        extracurrency: { 100: currencyAmount },
        body: beginCell().storeUint(0, 32).storeStringTail('Hrum hrum').endCell(),
    });
    ui.write('Sent');
}
