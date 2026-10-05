import { Address, beginCell, Cell, SendMode, toNano } from '@ton/core';
import { compile, NetworkProvider } from '@ton/blueprint';
import { WalletSpam } from '../wrappers/WalletSpam';
import { JettonWallet } from '../wrappers/JettonWallet';
import { JettonMinter } from '../wrappers/JettonMinter';
import { loadKeypairFromEnv, formatPublicKey } from './lib/mnemonic';
import { MINTERS } from './lib/addresses';

const minterAddress = MINTERS.pub4Alt; // PUB4 (alt)

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const keypair = await loadKeypairFromEnv(ui);
    if (!keypair) return;
    ui.write(`pubkey: ${formatPublicKey(keypair)}`);

    const config = {
        subwallet_id: 698983191,
        keypair: keypair,
        spam_msg: Cell.EMPTY,
    };

    const addr = provider.sender().address!;
    const wallet = provider.open(WalletSpam.createFromAddress(addr, config));

    const transferMsg = JettonWallet.transferMessage(
        1n,
        Address.parse('kQBRhRCZPZGNFv9YTH_JNcaSzPQBHWzLXB8rFSzrLIwwbJli'),
        addr,
        Cell.EMPTY,
        toNano('0.05'),
        beginCell()
            .storeUint(0, 32) // text op
            .storeStringTail('Test')
            .endCell(), // deposit comment emulation
    );
    const jwalletCode = await compile('JettonWallet');
    const jwallet = JettonWallet.createFromConfig(
        { owner: addr, master: minterAddress, code: jwalletCode, optimized: true },
        jwalletCode,
    );
    const jminter = provider.open(JettonMinter.createFromAddress(minterAddress));
    console.log("From minter:", await jminter.getWalletAddress(addr))
    await wallet.send({
        to: jwallet.address,
        value: toNano('0.15'),
        bounce: true,
        sendMode: SendMode.PAY_GAS_SEPARATELY,
        body: transferMsg,
        init: jwallet.init,
    });
    ui.write('Sent');
}
