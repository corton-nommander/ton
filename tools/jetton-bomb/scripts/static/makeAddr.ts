import { mnemonicToWalletKey } from "@ton/crypto";
import { WalletSpam, spamMsg } from '../../wrappers/WalletSpam';
import fs from 'fs/promises';
import { compile } from "@ton/blueprint";

async function main() {
    const mnemonic = await fs.readFile('mnemonic.txt', 'utf8');
    const keypair = await mnemonicToWalletKey(mnemonic.split(' '));
    const subwallet_id = 100000;
    const pubkey = '0x' + keypair.publicKey.toString('hex');

    const wallet = WalletSpam.createFromConfig({ subwallet_id: subwallet_id, keypair: keypair, spam_msg: spamMsg }, await compile('WalletSpam'));
    const addr = wallet.address;
    console.log(addr);
}

main();