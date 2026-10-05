// send external to walletSpam (signed message with no message refs)
//
// Usage:
//   yarn blueprint run sendExternal
//   SUBWALLET=0 yarn blueprint run sendExternal

import { compile, NetworkProvider } from '@ton/blueprint';
import { WalletSpam, spamMsg } from '../wrappers/WalletSpam';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const subwalletId = Number(process.env.SUBWALLET ?? 0);

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    ui.write(`Pubkey: ${formatPublicKey(keypair)}`);

    const code = await compile('WalletSpam');
    const wallet = provider.open(
        WalletSpam.createFromConfig({ subwallet_id: subwalletId, keypair, spam_msg: spamMsg }, code),
    );

    ui.write(`Wallet (subwallet=${subwalletId}): ${wallet.address.toString()}`);

    const seqno = await wallet.getSeqno();
    ui.write(`Seqno: ${seqno}`);

    const transfer = wallet.createTransfer({ messages: [], seqno });
    await wallet.sendSignExternal(transfer);

    ui.write('External sent');
}
