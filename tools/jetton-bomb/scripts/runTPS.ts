import { toNano } from '@ton/core';
import { StaticTps } from '../wrappers/StaticTps';
import { compile, NetworkProvider, UIProvider } from '@ton/blueprint';
import { promptAddress } from '../wrappers/ui-utils';

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();
    const address = await promptAddress('Address:', ui);
    const targetTPS = parseInt(await ui.input('Target TPS:'));
    const runAmount = toNano(await ui.input('Run amount (TON):'));
    ui.write(`Address: ${address}`);
    const staticTps = provider.open(StaticTps.createFromAddress(address));
    await staticTps.sendRun(provider.sender(), runAmount, targetTPS);
    ui.write(`Run sent.`);
}