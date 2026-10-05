import { Address, toNano } from '@ton/core';
import { compile, NetworkProvider } from '@ton/blueprint';
import { StaticTps } from '../wrappers/StaticTps';
import * as fs from 'fs';
import * as path from 'path';

// Mines 16 nonces (one per shard, identified by top 4 bits of address hash)
// and deploys a StaticTps contract for each. The nonce sits in the targetTps
// slot of initial state; sendRun overwrites it with the real targetTps later.
//
// Usage:
//   yarn blueprint run deployTPSShards
//   DEPLOY_VALUE=16 SHARD_BITS=4 OUTPUT=tps_shards.json yarn blueprint run deployTPSShards
//   SKIP_DEPLOY=1 yarn blueprint run deployTPSShards   # mine + save, no deploys

const DEFAULT_DEPLOY_VALUE = '16';
const DEFAULT_SHARD_BITS = 4;
const DEFAULT_OUTPUT = path.join(__dirname, '..', 'tps_shards.json');

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();
    const owner = provider.sender().address;
    if (!owner) throw new Error('Owner address is required');

    const deployValueTon = process.env.DEPLOY_VALUE || DEFAULT_DEPLOY_VALUE;
    const shardBits = Number(process.env.SHARD_BITS || DEFAULT_SHARD_BITS);
    const outputPath = process.env.OUTPUT || DEFAULT_OUTPUT;
    const skipDeploy = process.env.SKIP_DEPLOY === '1';
    const shardCount = 1 << shardBits;

    if (shardBits < 1 || shardBits > 8) {
        throw new Error('SHARD_BITS must be between 1 and 8');
    }

    const code = await compile('StaticTps');

    ui.write(`Owner: ${owner.toString()}`);
    ui.write(`Mining ${shardCount} nonces (top ${shardBits} bits of address)...`);

    const found = new Map<number, { nonce: number; address: Address }>();
    let nonce = 0;
    const mineStart = Date.now();
    while (found.size < shardCount) {
        const tps = StaticTps.createFromConfig({ ownerAddress: owner, nonce }, code);
        const shard = (tps.address.hash[0] >> (8 - shardBits)) & ((1 << shardBits) - 1);
        if (!found.has(shard)) {
            found.set(shard, { nonce, address: tps.address });
        }
        nonce++;
    }
    ui.write(`Mined in ${nonce} iterations (${Date.now() - mineStart}ms)`);

    const sorted = [...found.entries()].sort(([a], [b]) => a - b);
    const hexWidth = Math.ceil(shardBits / 4);
    for (const [shard, { nonce: n, address }] of sorted) {
        ui.write(
            `  shard 0x${shard.toString(16).padStart(hexWidth, '0')}: nonce=${n.toString().padStart(4)}  ${address.toString()}`,
        );
    }

    const result = {
        owner: owner.toString(),
        shardBits,
        shards: sorted.map(([shard, { nonce: n, address }]) => ({
            shard,
            nonce: n,
            address: address.toString(),
        })),
    };
    fs.writeFileSync(outputPath, JSON.stringify(result, null, 2));
    ui.write(`Saved mapping to ${outputPath}`);

    if (skipDeploy) {
        ui.write('SKIP_DEPLOY=1, not deploying');
        return;
    }

    const confirm = (await ui.input(`Deploy all ${shardCount} contracts with ${deployValueTon} TON each? (y/N) `))
        .trim()
        .toLowerCase();
    if (confirm !== 'y') {
        ui.write('Deployment skipped.');
        return;
    }

    const deployValue = toNano(deployValueTon);
    for (const [shard, { nonce: n, address }] of sorted) {
        ui.write(`Deploying shard 0x${shard.toString(16).padStart(hexWidth, '0')} (nonce=${n}): ${address.toString()}`);
        const tps = provider.open(StaticTps.createFromConfig({ ownerAddress: owner, nonce: n }, code));
        await tps.sendDeploy(provider.sender(), deployValue);
        await provider.waitForDeploy(address);
        ui.write(`  deployed`);
    }

    ui.write(`All ${shardCount} contracts deployed.`);
}
