import { Blockchain, BlockchainSnapshot, SandboxContract, TreasuryContract, printTransactionFees } from '@ton/sandbox';
import { Cell, SendMode, beginCell, toNano } from '@ton/core';
import { WalletR, MIN_BALANCE } from '../wrappers/WalletR';
import '@ton/test-utils';
import { compile } from '@ton/blueprint';
import { generateKeyPair } from 'crypto';
import { KeyPair, getSecureRandomBytes, keyPairFromSeed } from '@ton/crypto';
import { printReprChain } from '../wrappers/ui-utils';

describe('WalletR', () => {
    let code: Cell;

    let blockchain: Blockchain;
    let deployer: SandboxContract<TreasuryContract>;
    let walletR: SandboxContract<WalletR>;
    let keypair: KeyPair;
    let initialState: BlockchainSnapshot;

    beforeAll(async () => {
        code = await compile('WalletR');
        blockchain = await Blockchain.create();
        blockchain.now = 100;
        keypair = keyPairFromSeed(await getSecureRandomBytes(32));

        walletR = blockchain.openContract(WalletR.createFromConfig({ subwallet_id: 0, keypair: keypair }, code));

        deployer = await blockchain.treasury('deployer');

        const deployResult = await walletR.sendDeploy(deployer.getSender(), toNano('100.0'));

        expect(deployResult.transactions).toHaveTransaction({
            from: deployer.address,
            to: walletR.address,
            deploy: true,
            success: true,
        });
    });

    it('should be deployed', async () => {
        const state = await blockchain.getContract(walletR.address);
        expect(state.balance).toBeGreaterThan(toNano('99.9'));
        initialState = blockchain.snapshot();
    });

    it('should send something', async () => {
        // await blockchain.setVerbosityForAddress(walletR.address, { vmLogs: 'vm_logs_full' });
        const res = await walletR.send({
            to: deployer.address,
            value: toNano('1'),
            body: beginCell()
                .storeUint(0xd53276db, 32) // excesses
                .storeUint(0, 64)
                .endCell(),
            sendMode: SendMode.NONE,
        });
        expect(res.transactions).toHaveTransaction({
            from: walletR.address,
            to: deployer.address,
            success: true,
            op: 0xd53276db,
        });
    });

    it('should reproduce 10 more copies', async () => {
        const res = await walletR.sendReproduce(10);
        console.log(res.transactions.length);
        // expect(res.transactions).toHaveTransaction({
        //     from: undefined,
        //     to: walletR.address,
        //     success: true,
        //     outMessagesCount: 2,
        // });
        printReprChain(res.transactions);
        expect(res.transactions.length).toBe(11); // + external
    });

    async function checkCopies(copies: number, echo = true) {
        for (let i = 0; i <= copies; i++) {
            if (echo) console.log('checking subwallet ', i);
            const _walletR = WalletR.createFromConfig({ subwallet_id: i, keypair: keypair }, code);
            const _contract = await blockchain.getContract(_walletR.address);
            expect(_contract.balance).toBeGreaterThan(MIN_BALANCE - toNano('0.12'));
            if (i != 0 && i != copies)
                // 0th and last - edges have the excesses
                expect(_contract.balance).toBeLessThan(MIN_BALANCE + toNano('0.12'));
        }
    }

    it('should have 10 copies up to subwallet 10', async () => {
        await checkCopies(10);
    });

    let topupped: BlockchainSnapshot;
    it('should recreate and topup', async () => {
        await blockchain.loadFrom(initialState);
        const topupper = await blockchain.treasury('topupper', { balance: toNano('100000000') }); // 100M TON
        const res = await walletR.sendDeploy(topupper.getSender(), toNano('1000000')); // 1M TON
        expect(res.transactions).toHaveTransaction({
            from: topupper.address,
            to: walletR.address,
            success: true,
        });

        const { balance } = await blockchain.getContract(walletR.address);
        expect(balance).toBeGreaterThan(toNano('999999.99'));
        topupped = blockchain.snapshot();
    });

    it('should produce 1k copies', async () => {
        const res = await walletR.sendReproduce(1000);
        printReprChain(res.transactions);
        expect(res.transactions).toHaveTransaction({
            from: undefined,
            to: walletR.address,
            success: true,
            outMessagesCount: 1,
        });
        expect(res.transactions.length).toBe(1001);
    });

    it('should produce 5k copies', async () => {
        await blockchain.loadFrom(topupped);
        const res = await walletR.sendReproduce(5000);
        expect(res.transactions).toHaveTransaction({
            from: undefined,
            to: walletR.address,
            success: true,
            outMessagesCount: 1,
        });
        expect(res.transactions.length).toBe(5001);
        printReprChain(res.transactions);
    });

    it('should check 5k copies', async () => {
        await checkCopies(5000, false);
    });
});
