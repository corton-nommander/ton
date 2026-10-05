import { toNano } from '@ton/core';
import { Humpalot } from '../wrappers/Humpalot';
import { compile, NetworkProvider } from '@ton/blueprint';

// Deploy the humpalot hash-grinder and optionally kick off grinding.
//
// Env:
//   DEPLOY_VALUE   TON to fund the contract on deploy      (default 0.1)
//   GRIND_BUDGET   gas to burn per grind message; 0 = don't kick after deploy
//                  (set just below the network per-tx gas limit)   (default 0)
//   GRIND_VALUE    TON attached to the grind message        (default 1)
//   GRIND_LOOP     1 => self-continue (one message stays in flight)  (default 0)
//
// Example:
//   DEPLOY_VALUE=0.1 GRIND_BUDGET=950000 GRIND_VALUE=2 GRIND_LOOP=1 \
//     yarn blueprint run deployHumpalot
export async function run(provider: NetworkProvider) {
    const ui = provider.ui();
    const owner = provider.sender().address;
    if (!owner) throw new Error('Sender address is required');

    const deployValue = toNano(process.env.DEPLOY_VALUE || '0.1');
    const grindBudget = Number(process.env.GRIND_BUDGET || '0');
    const grindValue = toNano(process.env.GRIND_VALUE || '1');
    const grindLoop = process.env.GRIND_LOOP === '1';

    const hump = provider.open(Humpalot.createFromConfig({}, await compile('Humpalot')));

    ui.write(`Deploying humpalot at: ${hump.address.toString()}`);
    await hump.sendDeploy(provider.sender(), deployValue);
    await provider.waitForDeploy(hump.address);
    ui.write(`Deployed. rounds_total=${await hump.getRoundsTotal()}`);

    if (grindBudget > 0) {
        ui.write(`Kicking grind: budget=${grindBudget} gas, value=${grindValue}, loop=${grindLoop}`);
        await hump.sendGrind(provider.sender(), grindValue, grindBudget, grindLoop);
        ui.write(grindLoop ? 'Self-continuing grind chain started.' : 'One grind burst sent.');
    } else {
        ui.write('Deployed only (GRIND_BUDGET=0). Drive it with the Go spammer or set GRIND_BUDGET to kick.');
    }

    ui.write(`humpalot: ${hump.address.toString()}`);
}
