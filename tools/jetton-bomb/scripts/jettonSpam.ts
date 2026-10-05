import { Cell, OpenedContract, SendMode, beginCell, toNano } from '@ton/core';
import { compile, NetworkProvider, sleep } from '@ton/blueprint';
import { WalletSpam, spamMsg } from '../wrappers/WalletSpam';
import { jettonContentToCell, JettonMinter } from '../wrappers/JettonMinter';
import { JettonWallet } from '../wrappers/JettonWallet';
import { randomAddress } from '@ton/test-utils';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';
import { MINTERS } from './lib/addresses';

// Usage:
//   COUNT=1000 FREQ=10 yarn blueprint run jettonSpam
//   COUNT=1000 PERIOD=200 yarn blueprint run jettonSpam
//   MINTER=pub4|classic  (default: pub4)
//   ONLY_EXT=1 COUNT=1000 FREQ=10 yarn blueprint run jettonSpam   # send only signed externals (no jetton transfer)

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const envCount = process.env.COUNT;
    const envFreq = process.env.FREQ;
    const envPeriod = process.env.PERIOD;
    const envMinter = process.env.MINTER;
    const onlyExt = process.env.ONLY_EXT === '1';

    if (!envCount) {
        ui.write('COUNT is required');
        return;
    }

    if (envFreq && envPeriod) {
        ui.write('FREQ and PERIOD are mutually exclusive');
        return;
    }

    const count = Number(envCount);
    const periodMs = envFreq ? Math.floor(1000 / Number(envFreq)) : Number(envPeriod || 100);

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    ui.write(`Pubkey: ${formatPublicKey(keypair)}`);

    const code = await compile('WalletSpam');

    let walletCode: Cell | undefined;
    let minterAddr: import('@ton/core').Address | undefined;
    let isOptimized = true;

    if (!onlyExt) {
        // const minterAddr = envMinter === 'classic' ? MINTERS.classic : MINTERS.pub4;
        // const isOptimized = minterAddr !== MINTERS.classic;

        // Deploy jetton minter if not yet deployed
        const minterCode = await compile('JettonMinter');
        walletCode = await compile('JettonWallet');
        const content = jettonContentToCell({
            type: 1,
            uri: 'https://gist.githubusercontent.com/1IxI1/a49277da081f6e27811b6d5593730286/raw/12b434408399a1500f2f81a668f95a484b4b2f1b/pub4.json',
        });
        const admin = provider.sender().address!;
        const minter = provider.open(
            JettonMinter.createFromConfig({ admin, content, wallet_code: walletCode }, minterCode),
        );
        minterAddr = minter.address;

        ui.write(`Minter: ${minterAddr.toString()}`);
    } else {
        ui.write('ONLY_EXT mode: sending signed externals with no message refs');
    }

    // const jminter = provider.open(JettonMinter.createFromAddress(minterAddr));
    // const jwalletCode = (await jminter.getJettonData()).walletCode;

    // ui.write(`Not checking minter, just using hardcoded jwallet code, hash ${walletCode.hash().toString('hex')}`);

    ui.write(`Count: ${count}, Period: ${periodMs}ms (${(1000 / periodMs).toFixed(1)} req/s)`);

    let sent = 0;
    let failed = 0;

    const wallets = new Map<number, OpenedContract<WalletSpam>>();
    const seqnos = new Map<number, number>();

    function getWallet(id: number): OpenedContract<WalletSpam> {
        let w = wallets.get(id);
        if (!w) {
            w = provider.open(
                WalletSpam.createFromConfig({ subwallet_id: id, keypair: keypair!, spam_msg: spamMsg }, code),
            );
            wallets.set(id, w);
        }
        return w;
    }

    async function sendFrom(id: number) {
        const wallet = getWallet(id);

        let seqno = seqnos.get(id);
        if (seqno === undefined) {
            seqno = await wallet.getSeqno();
        }
        seqnos.set(id, seqno + 1); // optimistic bump before send

        try {
            if (onlyExt) {
                const transfer = wallet.createTransfer({ messages: [], seqno });
                await wallet.sendSignExternal(transfer);
                return;
            }

            const dest = randomAddress();
            const comment = Math.floor(Math.random() * 1000000).toString();

            const transferMsg = JettonWallet.transferMessage(
                1n,
                dest,
                wallet.address,
                Cell.EMPTY,
                toNano('0.05'),
                beginCell().storeUint(0, 32).storeStringTail(comment).endCell(),
            );

            const jwallet = JettonWallet.createFromConfig(
                { owner: wallet.address, master: minterAddr!, code: walletCode!, optimized: isOptimized },
                walletCode!,
            );

            await wallet.send(
                {
                    to: jwallet.address,
                    value: toNano('0.16'),
                    bounce: true,
                    sendMode: SendMode.PAY_GAS_SEPARATELY,
                    body: transferMsg,
                    init: jwallet.init,
                },
                seqno,
            );
        } catch (e) {
            seqnos.delete(id); // unknown state — refetch next time
            throw e;
        }
    }

    ui.write('Starting spam loop...');

    while (true) {
        const id = Math.floor(Math.random() * count);
        sendFrom(id)
            .then(() => {
                sent++;
            })
            .catch((e) => {
                failed++;
            });
        if ((sent + failed) % 100 === 0 && sent + failed > 0) {
            console.log(`sent: ${sent}, failed: ${failed}`);
        }
        await sleep(periodMs);
    }
}
