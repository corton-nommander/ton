import { Address, beginCell, Cell, storeStateInit } from '@ton/core';
import { StaticTps, Opcodes } from '../wrappers/StaticTps';
import * as fs from 'fs';
import * as path from 'path';

function loadCode(): Cell {
    const compiled = JSON.parse(
        fs.readFileSync(path.join(__dirname, '..', 'build', 'StaticTps.compiled.json'), 'utf-8'),
    );
    return Cell.fromBoc(Buffer.from(compiled.hex, 'hex'))[0];
}

function getContract(ownerAddr: Address) {
    return StaticTps.createFromConfig({ ownerAddress: ownerAddr }, loadCode());
}

function start(ownerAddr: Address, tps: number) {
    const contract = getContract(ownerAddr);
    const stateInitCell = beginCell().store(storeStateInit(contract.init!)).endCell();
    const body = beginCell().storeUint(Opcodes.run, 32).storeUint(tps, 32).endCell();

    console.log(`Address: ${contract.address.toString()}`);
    console.log(`StateInit BOC: ${stateInitCell.toBoc().toString('base64')}`);
    console.log(`Run body BOC: ${body.toBoc().toString('base64')}`);
}

function stop(ownerAddr: Address) {
    const contract = getContract(ownerAddr);
    const body = beginCell().storeUint(Opcodes.stop, 32).endCell();

    console.log(`Address: ${contract.address.toString()}`);
    console.log(`Stop body BOC: ${body.toBoc().toString('base64')}`);
}

// CLI
const args = process.argv.slice(2);
if (args.length < 2) {
    console.error('Usage:');
    console.error('  ts-node scripts/tpsCli.ts <owner-addr> start <tps>');
    console.error('  ts-node scripts/tpsCli.ts <owner-addr> stop');
    process.exit(1);
}

const ownerAddr = Address.parse(args[0]);
const command = args[1];

if (command === 'start') {
    const tps = parseInt(args[2]);
    if (isNaN(tps) || tps <= 0) {
        console.error('Error: <tps> must be a positive integer');
        process.exit(1);
    }
    start(ownerAddr, tps);
} else if (command === 'stop') {
    stop(ownerAddr);
} else {
    console.error(`Unknown command: ${command}. Use "start" or "stop".`);
    process.exit(1);
}
