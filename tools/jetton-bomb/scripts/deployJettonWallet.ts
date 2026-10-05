import { toNano } from '@ton/core';
import { JettonWallet } from '../wrappers/JettonWallet';
import { compile, NetworkProvider } from '@ton/blueprint';
import { promptAddress, promptBool } from '../wrappers/ui-utils';
import { MINTERS } from './lib/addresses';

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();
    const owner = await promptAddress('Owner address:', ui, provider.sender().address);
    const master = await promptAddress('Minter address:', ui, MINTERS.pub4);
    const optimized = await promptBool('Use optimized wallet? (y/n)', ['y', 'n'], ui);

    const walletCode = await compile('JettonWallet');
    const jettonWallet = JettonWallet.createFromConfig({ owner, master, code: walletCode, optimized }, walletCode);

    await provider.deploy(jettonWallet, toNano('0.05'));

    const openedContract = provider.open(jettonWallet);

    // run methods on `openedContract`
}
