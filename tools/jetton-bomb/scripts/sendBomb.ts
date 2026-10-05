import { Cell, SendMode, beginCell, toNano } from '@ton/core';
import { compile, NetworkProvider, sleep } from '@ton/blueprint';
import fss from 'fs';
import { JettonWallet } from '../wrappers/JettonWallet';
import { spamMsg, WalletSpam } from '../wrappers/WalletSpam';
import { JettonMinter } from '../wrappers/JettonMinter';
import { getRandDest } from './destinations';
import { randomAddress } from '@ton/test-utils';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';
import { MINTERS } from './lib/addresses';

const isJettonOptimized = true;
const isEC = false;
const withInit = true;
const fullRandom = true;

export const minter = isJettonOptimized ? MINTERS.pub4 : MINTERS.classic;

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    const keypairSafe = keypair;
    ui.write(`Pubkey: ${formatPublicKey(keypairSafe)}`);

    const code = await compile('WalletSpam');
    const wallet = provider.open(
        WalletSpam.createFromConfig({ subwallet_id: 0, keypair: keypairSafe, spam_msg: spamMsg }, code),
    );
    const firstWallet = wallet.address;
    ui.write(`0th WalletSpam address: ${firstWallet.toString()}`);

    const jminter = provider.open(JettonMinter.createFromAddress(minter));
    const jwalletCode = (await jminter.getJettonData()).walletCode;

    const last = Number(await ui.input('Last subwallet id:'));

    var stream = fss.createWriteStream('failed.txt', { flags: 'a' });

    async function send(i: number) {
        let _walletR = provider.open(
            WalletSpam.createFromConfig({ subwallet_id: i, keypair: keypairSafe, spam_msg: spamMsg }, code),
        );
        const randomComment = Math.floor(Math.random() * 1000000).toString();
        const dest = fullRandom ? randomAddress() : getRandDest();
        try {
            if (isEC) {
                await _walletR.send({
                    to: dest,
                    value: toNano('0.01'),
                    extracurrency: { 100: 100000000n },
                    bounce: false,
                    sendMode: SendMode.PAY_GAS_SEPARATELY,
                    body: beginCell()
                        .storeUint(0, 32) // text op
                        .storeStringTail('EC ' + randomComment)
                        .endCell(), // deposit comment emulation
                });
                ui.write(`sending EC from ${_walletR.address.toString()}, ${i}`);
            } else {
                const transferMsg = JettonWallet.transferMessage(
                    1n,
                    dest,
                    _walletR.address,
                    Cell.EMPTY,
                    toNano('0.05'),
                    beginCell()
                        .storeUint(0, 32) // text op
                        .storeStringTail(randomComment)
                        .endCell(), // deposit comment emulation
                );
                const jwallet = JettonWallet.createFromConfig(
                    { owner: _walletR.address, master: minter, code: jwalletCode, optimized: isJettonOptimized },
                    jwalletCode,
                );
                // const jwa = await jminter.getWalletAddress(_walletR.address);
                // console.log('From minter:', jwa);
                // console.log('In code', jwallet.address);
                await _walletR.send({
                    to: jwallet.address,
                    value: toNano('0.16'),
                    bounce: true,
                    sendMode: SendMode.PAY_GAS_SEPARATELY,
                    body: transferMsg,
                    init: withInit ? jwallet.init : undefined,
                });
                ui.write(`sending jettons from ${_walletR.address.toString()}, ${i}`);
            }
        } catch (e) {
            // throw e;
            ui.write(`failed to send to ${_walletR.address.toString()}, ${i} error: ${e}`);
            stream.write('\n' + i);
        }
    }
    const repeatInput = (await ui.input('Repeat forever? (y/n) [y]')).trim().toLowerCase();
    const repeat = repeatInput === '' || repeatInput === 'y';

    do {
        for (let i = 46000; i <= last; i++) {
            send(i);
            await sleep(20);
        }
    } while (repeat);
    ui.write('Sending & sleeping...');
    await sleep(60000);
    stream.end();
    // if (failed.length > 0) {
    //     ui.write(`Writing ${failed.length} fails...`);
    //     await fs.writeFile('failed.txt', failed.join('\n'));
    // }
}
