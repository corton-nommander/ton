import { Blockchain, SandboxContract, SendMessageResult, TreasuryContract } from '@ton/sandbox';
import { beginCell, Cell, toNano } from '@ton/core';
import { compile } from '@ton/blueprint';
import '@ton/test-utils';
import { Humpalot } from '../wrappers/Humpalot';
import { Fatbastard } from '../wrappers/Fatbastard';

describe('Load-test contracts', () => {
    let humpCode: Cell;
    let fatCode: Cell;

    beforeAll(async () => {
        humpCode = await compile('Humpalot');
        fatCode = await compile('Fatbastard');
    });

    describe('Humpalot (hash grinder)', () => {
        let bc: Blockchain;
        let deployer: SandboxContract<TreasuryContract>;
        let hump: SandboxContract<Humpalot>;

        beforeEach(async () => {
            bc = await Blockchain.create();
            deployer = await bc.treasury('deployer');
            hump = bc.openContract(Humpalot.createFromConfig({}, humpCode));
            await hump.sendDeploy(deployer.getSender(), toNano('1'));
        });

        const humpGas = (r: SendMessageResult, addr = hump.address) => {
            const tx = r.transactions.find(
                (t) =>
                    t.description.type === 'generic' &&
                    t.inMessage?.info.type === 'internal' &&
                    t.inMessage.info.dest.equals(addr),
            );
            // @ts-expect-error compute phase shape
            return Number(tx!.description.computePhase.gasUsed as bigint);
        };

        it('fixed-rounds grind burns gas proportional to rounds and advances the min hash', async () => {
            const gasOf = async (rounds: number) =>
                humpGas(await hump.sendGrindRounds(deployer.getSender(), toNano('1'), rounds));
            const g100 = await gasOf(100);
            const g1000 = await gasOf(1000);
            expect(g1000).toBeGreaterThan(g100 * 5); // roughly linear in rounds
            expect(await hump.getRoundsTotal()).toBeGreaterThanOrEqual(1100n);
            expect(await hump.getBestHash()).toBeLessThan((1n << 256n) - 1n);
        });

        it('budget grind stops near the requested gas and adapts to the target', async () => {
            const g200k = humpGas(await hump.sendGrind(deployer.getSender(), toNano('5'), 200_000));
            const g600k = humpGas(await hump.sendGrind(deployer.getSender(), toNano('5'), 600_000));
            // consumed lands at/just past the budget (bounded overshoot = one batch)
            expect(g200k).toBeGreaterThanOrEqual(200_000);
            expect(g200k).toBeLessThan(215_000);
            expect(g600k).toBeGreaterThanOrEqual(600_000);
            expect(g600k).toBeLessThan(615_000);
        });

        it('self-continues exactly once per grind_loop message', async () => {
            const r = await hump.sendGrind(deployer.getSender(), toNano('1'), 50_000, true);
            // one message out, back to self
            expect(r.transactions.length).toBeGreaterThanOrEqual(2);
            expect(await hump.getSeqno()).toBeGreaterThanOrEqual(1);
        });
    });

    describe('Fatbastard (fat state)', () => {
        let bc: Blockchain;
        let deployer: SandboxContract<TreasuryContract>;
        let fat: SandboxContract<Fatbastard>;

        beforeEach(async () => {
            bc = await Blockchain.create();
            deployer = await bc.treasury('deployer');
            fat = bc.openContract(Fatbastard.createFromConfig({}, fatCode));
            await fat.sendDeploy(deployer.getSender(), toNano('1'));
        });

        it('grows state cells and reports the hint', async () => {
            await fat.sendGrow(deployer.getSender(), toNano('1'), 20, 15);
            expect(await fat.getNextKey()).toBe(20n);
            // 20 entries * (15 chain cells + 1 leaf) = 320
            expect(await fat.getCellsHint()).toBe(320n);
        });

        it('grow stops at the gas budget and commits partial progress', async () => {
            // ask for far more than a budget can build; expect it to stop early
            await fat.sendGrow(deployer.getSender(), toNano('10'), 100000, 18, 700000);
            const added = await fat.getNextKey();
            expect(added).toBeGreaterThan(0n); // committed something
            expect(added).toBeLessThan(100000n); // but not the full requested count
        });

        it('poke mutates the nonce (busts the storage-dict cache) without growing', async () => {
            await fat.sendGrow(deployer.getSender(), toNano('1'), 10, 10);
            const keyBefore = await fat.getNextKey();
            const nonceBefore = await fat.getNonce();
            await fat.sendPoke(deployer.getSender(), toNano('0.1'));
            expect(await fat.getNonce()).toBe(nonceBefore + 1n);
            expect(await fat.getNextKey()).toBe(keyBefore); // poke does not grow
        });

        it('actually produces a large, deep account state (serialized)', async () => {
            // many grows -> thousands of cells, like the observed 9k-cell grinders
            for (let i = 0; i < 15; i++) {
                await fat.sendGrow(deployer.getSender(), toNano('2'), 30, 18);
            }
            expect(await fat.getNextKey()).toBe(450n);
            const cells = await fat.getCellsHint();
            expect(cells).toBe(450n * 19n); // 8550 cells

            const acc = (await bc.getContract(fat.address)).account.account;
            const stateBoc = beginCell()
                .storeRef(acc!.storage.state.type === 'active' ? acc!.storage.state.state.data! : beginCell().endCell())
                .endCell();
            expect(stateBoc.toBoc().length).toBeGreaterThan(5000); // real bytes on disk
        });
    });
});
