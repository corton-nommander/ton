import { Address, toNano } from '@ton/core';
import { PingPong } from '../wrappers/PingPong';
import { compile, NetworkProvider, UIProvider } from '@ton/blueprint';
import { promptAddress, promptBool } from '../wrappers/ui-utils';

async function promptNano(ui: UIProvider, label: string, fallback: string): Promise<bigint> {
    const input = (await ui.input(`${label} [${fallback}]`)).trim();
    return toNano(input === '' ? fallback : input);
}

async function promptLimit(ui: UIProvider, fallback: string): Promise<bigint> {
    const input = (await ui.input(`Enter limit (iterations) [${fallback}]`)).trim();
    return BigInt(input === '' ? fallback : input);
}

async function openPingPong(provider: NetworkProvider, ui: UIProvider, ownerAddress?: Address) {
    const nulladdr = Address.parse('0:0000000000000000000000000000000000000000000000000000000000000000');
    const addr = await promptAddress('PingPong address:', ui, nulladdr);
    if (addr.equals(nulladdr)) { 
        return provider.open(PingPong.createFromConfig({
            ownerAddress: ownerAddress ?? nulladdr,
            isRunning: false,
            counter: 0n,
            limit: 0n,
        }, await compile('PingPong')));
    }
    return provider.open(PingPong.createFromAddress(addr));
}

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();
    const ownerAddress = provider.sender().address;

    if (!ownerAddress) {
        throw new Error('Owner address is required');
    }

    const action = await ui.choose('PingPong action', ['Deploy new', 'Run existing', 'Stop existing', 'Status'], (c) => c);

    if (action === 'Deploy new') {
        const pingPong = provider.open(
            PingPong.createFromConfig(
                {
                    ownerAddress,
                    isRunning: false,
                    counter: 0n,
                    limit: 0n,
                },
                await compile('PingPong'),
            ),
        );

        const deployAmount = await promptNano(ui, 'Deploy amount (TON)', '0.05');
        await pingPong.sendDeploy(provider.sender(), deployAmount);
        await provider.waitForDeploy(pingPong.address);

        ui.write(`PingPong contract deployed at: ${pingPong.address.toString()}`);
        ui.write(`Owner: ${ownerAddress.toString()}`);

        const shouldRun = await promptBool('Run now? (y/n)', ['y', 'n'], ui);
        if (shouldRun) {
            const runAmount = await promptNano(ui, 'Run amount (TON)', '0.1');
            const limit = await promptLimit(ui, '150');
            await pingPong.sendRun(provider.sender(), runAmount, limit);
            ui.write(`Run started. Limit: ${limit.toString()}`);
        }
        return;
    }

    const pingPong = await openPingPong(provider, ui, ownerAddress);

    if (action === 'Run existing') {
        const runAmount = await promptNano(ui, 'Run amount (TON)', '0.1');
        const limit = await promptLimit(ui, '150');
        await pingPong.sendRun(provider.sender(), runAmount, limit);
        ui.write(`Run started. Limit: ${limit.toString()}`);
        return;
    }

    if (action === 'Stop existing') {
        const stopAmount = await promptNano(ui, 'Stop amount (TON)', '0.05');
        await pingPong.sendStop(provider.sender(), stopAmount);
        ui.write('Stopped.');
        return;
    }

    const [isRunning, owner, counter, limit] = await pingPong.getPingPongInfo();
    ui.write(`PingPong address: ${pingPong.address.toString()}`);
    ui.write(`Owner: ${owner.toString()}`);
    ui.write(`Running: ${isRunning ? 'yes' : 'no'}`);
    ui.write(`Counter: ${counter.toString()}`);
    ui.write(`Limit: ${limit.toString()}`);
}
