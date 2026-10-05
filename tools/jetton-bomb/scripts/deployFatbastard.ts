import { toNano } from '@ton/core';
import { Fatbastard } from '../wrappers/Fatbastard';
import { compile, NetworkProvider } from '@ton/blueprint';

// Deploy the fatbastard fat-state contract and inflate its storage.
//
// The load itself is the stream of cheap `poke` messages (drive those with the
// Go spammer); this script does the one-time setup: deploy + grow the account
// to a target cell count so that every later poke forces a full storage-dict
// re-walk in the collator (prelim_storage_stat).
//
// Env:
//   DEPLOY_VALUE       TON to fund on deploy                     (default 0.1)
//   GROW_TARGET_CELLS  grow until cells_hint >= this; 0 = deploy only (default 0)
//   GROW_DEPTH         cells per entry (tree depth)              (default 18)
//   GROW_COUNT         max entries per grow message              (default 30)
//   GROW_VALUE         TON attached to each grow message         (default 2)
//   GROW_BUDGET        gas cap per grow message (stops early)    (default 900000)
//   POKE_COUNT         send this many poke messages after growing (default 0)
//   POKE_VALUE         TON per poke                              (default 0.05)
//
// Example (build ~9000 cells at depth 18, then leave it for the spammer):
//   DEPLOY_VALUE=0.2 GROW_TARGET_CELLS=9000 GROW_DEPTH=18 GROW_COUNT=30 \
//     yarn blueprint run deployFatbastard
export async function run(provider: NetworkProvider) {
    const ui = provider.ui();
    const owner = provider.sender().address;
    if (!owner) throw new Error('Sender address is required');

    const deployValue = toNano(process.env.DEPLOY_VALUE || '0.1');
    const targetCells = BigInt(process.env.GROW_TARGET_CELLS || '0');
    const growDepth = Number(process.env.GROW_DEPTH || '18');
    const growCount = Number(process.env.GROW_COUNT || '30');
    const growValue = toNano(process.env.GROW_VALUE || '2');
    const growBudget = Number(process.env.GROW_BUDGET || '900000');
    const pokeCount = Number(process.env.POKE_COUNT || '0');
    const pokeValue = toNano(process.env.POKE_VALUE || '0.05');

    const fat = provider.open(Fatbastard.createFromConfig({}, await compile('Fatbastard')));

    ui.write(`Deploying fatbastard at: ${fat.address.toString()}`);
    await fat.sendDeploy(provider.sender(), deployValue);
    await provider.waitForDeploy(fat.address);
    ui.write(`Deployed. cells_hint=${await fat.getCellsHint()}`);

    if (targetCells > 0n) {
        ui.write(`Growing to >= ${targetCells} cells (depth ${growDepth}, ${growCount}/msg)...`);
        let cells = await fat.getCellsHint();
        let stalls = 0;
        while (cells < targetCells) {
            const before = await fat.getNextKey();
            await fat.sendGrow(provider.sender(), growValue, growCount, growDepth, growBudget);
            // wait for the grow to land: getNextKey must advance
            let after = before;
            for (let i = 0; i < 30 && after === before; i++) {
                await sleep(1500);
                after = await fat.getNextKey();
            }
            cells = await fat.getCellsHint();
            ui.write(`  cells_hint=${cells}  next_key=${after}`);
            if (after === before) {
                stalls++;
                if (stalls >= 3) {
                    ui.write('Grow stalled (no progress in 3 tries) — stopping. Check wallet balance / gas.');
                    break;
                }
            } else {
                stalls = 0;
            }
        }
        ui.write(`Grow done. cells_hint=${await fat.getCellsHint()}`);
    } else {
        ui.write('Deployed only (GROW_TARGET_CELLS=0).');
    }

    for (let i = 0; i < pokeCount; i++) {
        await fat.sendPoke(provider.sender(), pokeValue);
        ui.write(`poke ${i + 1}/${pokeCount} sent`);
    }

    ui.write(`fatbastard: ${fat.address.toString()}`);
}

function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}
