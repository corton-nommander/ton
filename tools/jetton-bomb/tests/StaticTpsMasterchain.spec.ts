import { Blockchain, internal } from '@ton/sandbox';
import { Address, beginCell, Cell, toNano } from '@ton/core';
import { compile } from '@ton/blueprint';
import { Opcodes, StaticTps } from '../wrappers/StaticTps';

describe('StaticTps work-message funding', () => {
    let code: Cell;

    beforeAll(async () => {
        code = await compile('StaticTps');
    });

    it.each([-1, 0])('executes self-work at workchain %s fee rates', async (workchain) => {
        const blockchain = await Blockchain.create();
        blockchain.now = 1_700_000_000;
        const owner = Address.parseRaw(`${workchain}:${'12'.repeat(32)}`);
        const target = StaticTps.createFromConfig({ ownerAddress: owner }, code, workchain);
        const opened = blockchain.openContract(target);

        await blockchain.sendMessage(
            internal({
                from: owner,
                to: target.address,
                stateInit: target.init,
                value: toNano('1000000'),
                bounce: false,
            }),
        );

        // Iterate a bounded prefix; the original heartbeat deliberately keeps
        // emitting until stopped and must not be drained by an ordinary send.
        const iterator = await blockchain.sendMessageIter(
            internal({
                from: owner,
                to: target.address,
                value: toNano('1'),
                body: beginCell().storeUint(Opcodes.run, 32).storeUint(10, 32).endCell(),
            }),
        );
        await iterator.next();
        blockchain.now += 6; // First bounce now opens the initial work window.

        let workReceipts = 0;
        for (let step = 0; step < 20 && workReceipts < 2; step++) {
            const next = await iterator.next();
            if (next.done) break;
            const tx = next.value;
            const input = tx.inMessage;
            if (
                input?.info.type !== 'internal' ||
                !input.info.src.equals(target.address) ||
                !input.info.dest.equals(target.address)
            )
                continue;
            const body = input.body.beginParse();
            if (body.remainingBits < 32 || body.loadUint(32) !== Opcodes.work) continue;
            expect(tx.description.type).toBe('generic');
            if (tx.description.type === 'generic') expect(tx.description.aborted).toBe(false);
            // Mode 2 subtracts forwarding fees from the attached float. The
            // masterchain gets sufficient float; basechain retains 0.003 TON.
            if (workchain === -1) expect(input.info.value.coins).toBeGreaterThan(toNano('0.003'));
            else expect(input.info.value.coins).toBeLessThanOrEqual(toNano('0.003'));
            workReceipts++;
        }

        expect(workReceipts).toBe(2);
        expect(await opened.getTotalTxCount()).toBe(2n);
    });
});
