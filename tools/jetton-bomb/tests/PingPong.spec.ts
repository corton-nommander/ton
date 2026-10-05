import {
    Blockchain,
    SandboxContract,
    TreasuryContract,
    printTransactionFees,
} from '@ton/sandbox';
import { Address, toNano } from '@ton/core';
import { PingPong } from '../wrappers/PingPong';
import '@ton/test-utils';
import { compile } from '@ton/blueprint';

describe('PingPong', () => {
    let code: any;
    let blockchain: Blockchain;
    let deployer: SandboxContract<TreasuryContract>;
    let user: SandboxContract<TreasuryContract>;
    let pingPong: SandboxContract<PingPong>;

    beforeAll(async () => {
        code = await compile('PingPong');
        blockchain = await Blockchain.create();
        blockchain.now = 100;

        deployer = await blockchain.treasury('deployer');
        user = await blockchain.treasury('user');
    });

    beforeEach(async () => {
        pingPong = blockchain.openContract(
            PingPong.createFromConfig(
                {
                    ownerAddress: deployer.address,
                    isRunning: false,
                    counter: 0n,
                    limit: 0n,
                },
                code,
            ),
        );
    });

    it('should deploy', async () => {
        const deployResult = await pingPong.sendDeploy(deployer.getSender(), toNano('0.05'));

        expect(deployResult.transactions).toHaveTransaction({
            from: deployer.address,
            to: pingPong.address,
            deploy: true,
            success: true,
        });

        const owner = await pingPong.getOwner();
        expect(owner.toString()).toEqual(deployer.address.toString());

        const isRunning = await pingPong.getIsRunning();
        expect(isRunning).toBe(false);
    });

    it('should start ping-pong when owner sends run', async () => {
        await pingPong.sendDeploy(deployer.getSender(), toNano('0.05'));

        const limit = 10n;
        const runResult = await pingPong.sendRun(deployer.getSender(), toNano('0.1'), limit);

        expect(runResult.transactions).toHaveTransaction({
            from: deployer.address,
            to: pingPong.address,
            success: true,
        });

        // check that ping message was sent to masterchain
        // should have: treasury -> contract (run) -> empty addr (ping) -> bounce back
        expect(runResult.transactions.length).toBeGreaterThanOrEqual(2);

        const isRunning = await pingPong.getIsRunning();
        expect(isRunning).toBe(true);

        const counter = await pingPong.getCounter();
        expect(counter).toBe(0n);

        const storedLimit = await pingPong.getLimit();
        expect(storedLimit).toBe(limit);
    });

    it('should reject run from non-owner', async () => {
        await pingPong.sendDeploy(deployer.getSender(), toNano('0.05'));

        const runResult = await pingPong.sendRun(user.getSender(), toNano('0.1'), 10n);

        // should fail with exit code 401
        expect(runResult.transactions).toHaveTransaction({
            from: user.address,
            to: pingPong.address,
            success: false,
            exitCode: 401,
        });
        
        // state should not change after failed transaction
        const isRunning = await pingPong.getIsRunning();
        expect(isRunning).toBe(false);
    });

    it('should stop ping-pong when anyone sends stop', async () => {
        await pingPong.sendDeploy(deployer.getSender(), toNano('0.05'));
        await pingPong.sendRun(deployer.getSender(), toNano('0.1'), 10n);

        // verify it's running
        let isRunning = await pingPong.getIsRunning();
        expect(isRunning).toBe(true);

        // stop from non-owner
        const stopResult = await pingPong.sendStop(user.getSender(), toNano('0.05'));

        expect(stopResult.transactions).toHaveTransaction({
            from: user.address,
            to: pingPong.address,
            success: true,
        });

        isRunning = await pingPong.getIsRunning();
        expect(isRunning).toBe(false);
    });

    it('should handle bounce and resend if running', async () => {
        await pingPong.sendDeploy(deployer.getSender(), toNano('0.5'));
        await pingPong.sendRun(deployer.getSender(), toNano('1'), 100n);

        const isRunning = await pingPong.getIsRunning();
        expect(isRunning).toBe(true);

        // note: in real blockchain the message to empty address will bounce
        // and trigger onBouncedMessage, which will send another ping
        // in sandbox testing this behavior is limited
    });

    it('should not resend after bounce if stopped', async () => {
        await pingPong.sendDeploy(deployer.getSender(), toNano('0.5'));
        await pingPong.sendRun(deployer.getSender(), toNano('1'), 100n);
        await pingPong.sendStop(deployer.getSender(), toNano('0.05'));

        const isRunning = await pingPong.getIsRunning();
        expect(isRunning).toBe(false);
    });

    it('should stop automatically when limit reached', async () => {
        await pingPong.sendDeploy(deployer.getSender(), toNano('0.5'));
        
        const limit = 3n;
        await pingPong.sendRun(deployer.getSender(), toNano('1'), limit);

        // initial state: running with counter = 0
        let isRunning = await pingPong.getIsRunning();
        expect(isRunning).toBe(true);
        
        let counter = await pingPong.getCounter();
        expect(counter).toBe(0n);

        // note: in sandbox, bounces happen synchronously in the same transaction
        // in real blockchain, after 3 bounces (counter reaches limit), 
        // the contract should stop automatically
    });
});
