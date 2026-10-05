import { compile, NetworkProvider } from '@ton/blueprint';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';
import { openWalletSpam } from './lib/wallets';
import { monitorDeployment } from './lib/monitor';
export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    const keypairSafe = keypair;
    ui.write(`Pubkey: ${formatPublicKey(keypairSafe)}`);

    const code = await compile('WalletSpam');
    async function getRWalletSeqno(subwallet: number) {
        try {
            const wallet = await openWalletSpam(provider, keypairSafe, subwallet, { code });
            return await wallet.getSeqno();
        } catch (e) {
            return -1;
        }
    }

    const target = Number(await ui.input('Enter the total count:'))!;
    await monitorDeployment(ui, target, getRWalletSeqno);
}
