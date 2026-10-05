import { toNano } from '@ton/core';
import { StaticTps } from '../wrappers/StaticTps';
import { compile, NetworkProvider, UIProvider } from '@ton/blueprint';

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();
    const ownerAddress = provider.sender().address;
    if (!ownerAddress) {
        throw new Error('Owner address is required');
    }

    const staticTps = provider.open(
        StaticTps.createFromConfig(
            { ownerAddress: provider.sender().address! },
            await compile('StaticTps'),
        ),
    );

    await staticTps.sendDeploy(provider.sender(), toNano('16'));

    await provider.waitForDeploy(staticTps.address);

    ui.write(`Owner: ${provider.sender().address}`);
    ui.write(`Contract: ${staticTps.address}`);

    const runAmount = toNano(await ui.input('Run amount (TON):'));
    const targetTps = parseInt(await ui.input('Target TPS:'));
    await staticTps.sendRun(provider.sender(), runAmount, targetTps);
    ui.write(`Run started. Target TPS: ${targetTps}. Run amount: ${runAmount}`);
}