import { Blockchain, SandboxContract, TreasuryContract } from '@ton/sandbox';
import { Cell, toNano } from '@ton/core';
import { StaticTps } from '../wrappers/StaticTps';
import '@ton/test-utils';
import { compile } from '@ton/blueprint';

describe('StaticTps', () => {
    let code: Cell;

    beforeAll(async () => {
        code = await compile('StaticTps');
    });

    let blockchain: Blockchain;
    let deployer: SandboxContract<TreasuryContract>;
    let staticTps: SandboxContract<StaticTps>;

    beforeEach(async () => {
        blockchain = await Blockchain.create();

        deployer = await blockchain.treasury('deployer');

        staticTps = blockchain.openContract(
            StaticTps.createFromConfig({ ownerAddress: deployer.address }, code),
        );

        const deployResult = await staticTps.sendDeploy(deployer.getSender(), toNano('10'));

        expect(deployResult.transactions).toHaveTransaction({
            from: deployer.address,
            to: staticTps.address,
            deploy: true,
            success: true,
        });
    });

    it('should deploy with correct initial state', async () => {
        expect(await staticTps.getIsRunning()).toBe(0);
        expect(await staticTps.getOwner()).toEqualAddress(deployer.address);
        expect(await staticTps.getTargetTps()).toBe(0);
        expect(await staticTps.getTotalTxCount()).toBe(0n);
        expect(await staticTps.getActiveEpoch()).toBe(0);
        expect(await staticTps.getMaxTps()).toBe(2083);
    });

    it('should start on Run message from owner', async () => {
        const result = await staticTps.sendRun(deployer.getSender(), toNano('5'), 10);

        expect(result.transactions).toHaveTransaction({
            from: deployer.address,
            to: staticTps.address,
            success: true,
        });

        expect(await staticTps.getIsRunning()).toBe(1);
        expect(await staticTps.getTargetTps()).toBe(10);
        expect(await staticTps.getActiveEpoch()).toBe(1);
    });

    it('should reject Run from non-owner', async () => {
        const hacker = await blockchain.treasury('hacker');

        const result = await staticTps.sendRun(hacker.getSender(), toNano('5'), 10);

        expect(result.transactions).toHaveTransaction({
            from: hacker.address,
            to: staticTps.address,
            success: false,
            exitCode: 401,
        });

        expect(await staticTps.getIsRunning()).toBe(0);
    });

    it('should stop on Stop message', async () => {
        await staticTps.sendRun(deployer.getSender(), toNano('5'), 10);
        expect(await staticTps.getIsRunning()).toBe(1);

        const stopResult = await staticTps.sendStop(deployer.getSender(), toNano('0.05'));

        expect(stopResult.transactions).toHaveTransaction({
            from: deployer.address,
            to: staticTps.address,
            success: true,
        });

        expect(await staticTps.getIsRunning()).toBe(0);
    });

    it('should allow anyone to stop', async () => {
        await staticTps.sendRun(deployer.getSender(), toNano('5'), 10);

        const stranger = await blockchain.treasury('stranger');
        const stopResult = await staticTps.sendStop(stranger.getSender(), toNano('0.05'));

        expect(stopResult.transactions).toHaveTransaction({
            from: stranger.address,
            to: staticTps.address,
            success: true,
        });

        expect(await staticTps.getIsRunning()).toBe(0);
    });

    it('should hot-update targetTps while running', async () => {
        await staticTps.sendRun(deployer.getSender(), toNano('5'), 10);
        expect(await staticTps.getTargetTps()).toBe(10);
        const firstEpoch = await staticTps.getActiveEpoch();

        await staticTps.sendRun(deployer.getSender(), toNano('0.05'), 50);
        expect(await staticTps.getTargetTps()).toBe(50);
        expect(await staticTps.getIsRunning()).toBe(1);
        expect(await staticTps.getActiveEpoch()).toBe(firstEpoch + 1);
    });

    it('should rotate epoch when stopped', async () => {
        await staticTps.sendRun(deployer.getSender(), toNano('5'), 10);
        const beforeStop = await staticTps.getActiveEpoch();

        await staticTps.sendStop(deployer.getSender(), toNano('0.05'));

        expect(await staticTps.getIsRunning()).toBe(0);
        expect(await staticTps.getActiveEpoch()).toBe(beforeStop + 1);
    });
});
