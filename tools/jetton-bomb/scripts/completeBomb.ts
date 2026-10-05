import { Address, Cell, SendMode, beginCell, toNano } from '@ton/core';
import { compile, NetworkProvider, sleep } from '@ton/blueprint';
import fss from 'fs';
import { JettonWallet } from '../wrappers/JettonWallet';
import { TonClient } from '@ton/ton';
import { WalletSpam, spamMsg } from '../wrappers/WalletSpam';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';
import { MINTERS } from './lib/addresses';
import fs from 'fs/promises';
const minterAddress = MINTERS.classic;
const withInit = false;

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();
    const deployer = provider.sender().address!;

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    const keypairSafe = keypair;
    ui.write(`Pubkey: ${formatPublicKey(keypairSafe)}`);

    const code = await compile('WalletSpam');
    const jwalletCode = await compile('JettonWallet');
    const wallet = provider.open(
        WalletSpam.createFromConfig({ subwallet_id: 0, keypair: keypairSafe, spam_msg: spamMsg }, code),
    );
    const firstWallet = wallet.address;
    ui.write(`0th WalletSpam address: ${firstWallet.toString()}`);

    var stream = fss.createWriteStream('failed-again.txt', { flags: 'a' });

    const file = await fs.readFile('failed-senders.txt', 'utf8');
    const addreses = file.split('\n');

    const api = provider.api();

    async function send(addr: Address) {
        if (!(api instanceof TonClient)) return;
        let data: Buffer | null = null;
        let balance: bigint = 0n;
        try {
            let res = await api.getContractState(addr);
            data = res.data;
            balance = res.balance;
        } catch (e) {
            ui.write(`Failed getting state for ${addr.toRawString()}: ${e}`);
            stream.write('\n' + addr.toRawString());
            return;
        }
        if (!data) {
            ui.write(`Failed getting data for ${addr.toRawString()}`);
            stream.write('\n' + addr.toRawString());
            return;
        }
        if (balance < toNano('0.9')) {
            ui.write(`To low balance ${balance} for ${addr.toRawString()}`);
            stream.write('\n' + addr.toRawString());
            return;
        }
        const ds = Cell.fromBoc(data)[0].beginParse();
        const seqno = ds.loadUint(32);
        const subwallet = ds.loadUint(32);
        let _walletR = provider.open(
            WalletSpam.createFromConfig({ subwallet_id: subwallet, keypair: keypairSafe, spam_msg: spamMsg }, code),
        );
        if (!_walletR.address.equals(addr)) {
            ui.write(`Address from file ${addr} is some kinda incorrect`);
            stream.write('\n' + addr.toRawString());
            return;
        }
        const randomComment = Math.floor(Math.random() * 1000000).toString();
        const transferMsg = JettonWallet.transferMessage(
            1n,
            deployer,
            _walletR.address,
            Cell.EMPTY,
            toNano('0.05'),
            beginCell()
                .storeUint(0, 32) // text op
                .storeStringTail(randomComment)
                .endCell(), // deposit comment emulation
        );
        const jwallet = JettonWallet.createFromConfig(
            { owner: _walletR.address, master: minterAddress, code: jwalletCode },
            jwalletCode,
        );
        try {
            await _walletR.send(
                {
                    to: jwallet.address,
                    value: toNano('0.15'),
                    sendMode: SendMode.PAY_GAS_SEPARATELY,
                    body: transferMsg,
                    init: withInit ? jwallet.init : undefined,
                },
                seqno,
            );
            ui.write(`sending to ${_walletR.address.toRawString()}, no.${subwallet}`);
        } catch (e) {
            ui.write(`failed to send to ${_walletR.address.toRawString()} error: ${e}`);
            stream.write('\n' + addr.toRawString());
        }
    }
    for (let addr of addreses) {
        send(Address.parseRaw(addr));
        await sleep(60);
    }
    ui.write('Sending & sleeping...');
    await sleep(60000);
    stream.end();
}
