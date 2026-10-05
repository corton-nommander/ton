import { Address, toNano } from '@ton/core';
import { NetworkProvider } from '@ton/blueprint';
import { JettonMinter } from '../wrappers/JettonMinter';
import { MINTERS } from './lib/addresses';

const toAddr = Address.parse('kQBJSvyUJqVEBPHEQqeOVoSXfJAaYdrc8dnGXJha4BCnhj_u');

export async function run(provider: NetworkProvider) {
    const jminter = provider.open(JettonMinter.createFromAddress(MINTERS.pub4));
    await jminter.sendMint(
        provider.sender(),
        toAddr,
        toNano('10'), // 10 billion, decimals = 0
        toNano('0.1'), // forward amount
        toNano('1'), // total
    );
}
