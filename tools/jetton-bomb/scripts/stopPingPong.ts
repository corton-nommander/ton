import { toNano } from '@ton/core';
import { StaticTps } from '../wrappers/StaticTps';
import { compile, NetworkProvider, UIProvider } from '@ton/blueprint';
import { promptAddress } from '../wrappers/ui-utils';

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();
    const address = await promptAddress('Address:', ui);

    ui.write(`Address: ${address}`);
    const pingPong = provider.open(StaticTps.createFromAddress(address));
    await pingPong.sendStop(provider.sender(), toNano('0.05'));
    ui.write(`Stop sent.`);
}