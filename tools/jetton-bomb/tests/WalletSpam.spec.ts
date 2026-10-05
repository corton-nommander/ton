import {
    Blockchain,
    BlockchainSnapshot,
    SandboxContract,
    TreasuryContract,
    printTransactionFees,
    BlockchainConfig,
} from '@ton/sandbox';
import { Address, Cell, MessageRelaxed, SendMode, beginCell, internal, storeMessageRelaxed, toNano, Message } from '@ton/core';
import { WalletSpam, MIN_BALANCE } from '../wrappers/WalletSpam';
import '@ton/test-utils';
import { compile } from '@ton/blueprint';
import { generateKeyPair } from 'crypto';
import { KeyPair, getSecureRandomBytes, keyPairFromSeed } from '@ton/crypto';
import { JettonMinter } from '../wrappers/JettonMinter';
import { JettonWallet } from '../wrappers/JettonWallet';

describe('JettonBomb', () => {
    let jWalletCode: Cell;
    let code: Cell;

    let blockchain: Blockchain;
    let deployer: SandboxContract<TreasuryContract>;
    let walletR: SandboxContract<WalletSpam>;
    let minter: SandboxContract<JettonMinter>;
    let keypair: KeyPair;
    let initialState: BlockchainSnapshot;
    let spamMsg: Cell;

    beforeAll(async () => {
        code = await compile('WalletSpam');
        blockchain = await Blockchain.create();
        blockchain.now = 100;
        keypair = keyPairFromSeed(await getSecureRandomBytes(32));

        deployer = await blockchain.treasury('deployer');

        spamMsg = beginCell()
            .storeUint(0x10, 6)
            .storeAddress(deployer.address)
            .storeCoins(toNano('0.01'))
            .storeUint(0, 1 + 4 + 4 + 64 + 32 + 1 + 1)
            .storeUint(0xd53276db, 32) // excesses
            .storeUint(0, 64)
            .endCell();

        walletR = blockchain.openContract(
            WalletSpam.createFromConfig({ subwallet_id: 0, keypair: keypair, spam_msg: spamMsg }, code),
        );

        const topupper = await blockchain.treasury('topupper', { balance: toNano('100000000') }); // 100M TON

        const msg = internal({
            to: topupper.address,
            value: toNano(1),
            extracurrency: { 100: toNano(10 ** 8) }, // 100M EC
            body: Cell.EMPTY,
        });
        let msgCell = beginCell();
        storeMessageRelaxed(msg)(msgCell);
        const msgFull = msg as Message;
        msgFull.info.src = deployer.address;
        const mintRes = await blockchain.sendMessage(msgFull);
        expect(mintRes.transactions).toHaveTransaction({ from: deployer.address, to: topupper.address });
        const contract = await blockchain.getContract(topupper.address);
        expect(contract.ec[100]).toBe(toNano(10 ** 8));

        let res = await walletR.sendDeploy(topupper.getSender(), toNano('1000000'), toNano('10000000')); // 1M TON, 10M EC

        expect(res.transactions).toHaveTransaction({
            from: topupper.address,
            to: walletR.address,
            deploy: true,
            success: true,
        });

        jWalletCode = await compile('JettonWallet');
        const jMinterCode = await compile('JettonMinter');

        minter = blockchain.openContract(
            JettonMinter.createFromConfig(
                { admin: deployer.address, content: Cell.EMPTY, wallet_code: jWalletCode },
                jMinterCode,
            ),
        );

        res = await minter.sendDeploy(deployer.getSender(), toNano('2.0'));
        expect(res.transactions).toHaveTransaction({
            from: deployer.address,
            to: minter.address,
            deploy: true,
            success: true,
        });
    });

    it('should deploy wallet', async () => {
        const state = await blockchain.getContract(walletR.address);
        expect(state.balance).toBeGreaterThan(toNano('999999.99'));
    });

    it('should deploy minter', async () => {
        const state = await blockchain.getContract(minter.address);
        expect(state.balance).toBeGreaterThan(toNano('1.9'));
        initialState = blockchain.snapshot();
    });

    async function checkCopies(copies: number) {
        for (let i = 0; i < copies; i++) {
            console.log('checking subwallet ', i);
            const _walletR = WalletSpam.createFromConfig(
                { subwallet_id: i, keypair: keypair, spam_msg: spamMsg },
                code,
            );
            const _contract = await blockchain.getContract(_walletR.address);
            expect(_contract.balance).toBeGreaterThan(MIN_BALANCE - toNano('0.1'));
            console.log(_contract.ec)
            expect(_contract.ec[100]).toBeGreaterThanOrEqual(MIN_BALANCE * 10n);
            if (i != 0 && i != copies - 1) expect(_contract.balance).toBeLessThanOrEqual(MIN_BALANCE);
        }
    }

    it('should produce 500 copies', async () => {
        await blockchain.setVerbosityForAddress(walletR.address, { vmLogs: 'vm_logs' });
        const contract = await blockchain.getContract(walletR.address);
        expect(contract.ec[100]).toBe(toNano(10000000));
        console.log('ec balance: ', contract.ec);
        const res = await walletR.sendReproduce(500, toNano(10000), undefined, toNano(100000)); // 5k copies, 10k TONs
        expect(res.transactions).toHaveTransaction({
            from: undefined,
            to: walletR.address,
            success: true,
            outMessagesCount: 1,
        });
        printTransactionFees(res.transactions);
        expect(res.transactions.length).toBe(500 * 2 + 1);
    });

    it('should check 500 copies', async () => {
        await checkCopies(500);
    });

    // let destJWAddr: Address;
    // let destJW: SandboxContract<JettonWallet>;
    // it('should transfer some jetton from one wallet', async () => {
    //     const jwallet = blockchain.openContract(
    //         JettonWallet.createFromConfig(
    //             { owner: walletR.address, master: minter.address, code: jWalletCode },
    //             jWalletCode,
    //         ),
    //     );
    //     const res = await walletR.send({
    //         to: jwallet.address,
    //         value: toNano('0.15'),
    //         init: jwallet.init,
    //         sendMode: SendMode.PAY_GAS_SEPARATELY,
    //         body: JettonWallet.transferMessage(
    //             10n,
    //             deployer.address,
    //             deployer.address,
    //             Cell.EMPTY,
    //             toNano('0.05'),
    //             Cell.EMPTY,
    //         ),
    //     });
    //     expect(res.transactions).toHaveTransaction({
    //         from: walletR.address,
    //         to: jwallet.address,
    //         success: true,
    //         deploy: true,
    //         outMessagesCount: 1,
    //     });
    //     destJWAddr = await minter.getWalletAddress(deployer.address);
    //     destJW = blockchain.openContract(JettonWallet.createFromAddress(destJWAddr));
    //     expect(await destJW.getJettonBalance()).toBe(100n + 10n); // 100 initial + 10 transferred
    // });

    // it('should make 500 transfers', async () => {
    //     const balanceBefore = await destJW.getJettonBalance();

    //     for (let i = 0; i < 500; i++) {
    //         const _walletR = blockchain.openContract(
    //             WalletSpam.createFromConfig({ subwallet_id: i, keypair: keypair, spam_msg: spamMsg }, code),
    //         );
    //         const jwallet = JettonWallet.createFromConfig(
    //             { owner: _walletR.address, master: minter.address, code: jWalletCode },
    //             jWalletCode,
    //         );
    //         const randomComment = Math.floor(Math.random() * 1000000).toString();
    //         const transferMsg = JettonWallet.transferMessage(
    //             20n,
    //             deployer.address,
    //             deployer.address,
    //             Cell.EMPTY,
    //             toNano('0.05'),
    //             beginCell()
    //                 .storeUint(0, 32) // text op
    //                 .storeStringTail(randomComment)
    //                 .endCell(), // deposit comment emulation
    //         );
    //         await _walletR.send({
    //             to: jwallet.address,
    //             value: toNano('0.15'),
    //             init: jwallet.init,
    //             sendMode: SendMode.PAY_GAS_SEPARATELY,
    //             body: transferMsg,
    //         });
    //     }
    //     const newBalance = await destJW.getJettonBalance();
    //     console.log('new balance: ', newBalance);
    //     expect(newBalance).toBe(balanceBefore + 500n * 20n);
    // });
});
