import { Address } from '@ton/core';
import { compile, NetworkProvider, sleep } from '@ton/blueprint';
import { WalletR } from '../wrappers/WalletR';
import { JettonWallet } from '../wrappers/JettonWallet';
import { TonClient } from '@ton/ton';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';
import { MINTERS } from './lib/addresses';

const minterAddress = MINTERS.classic;

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();
    const deployer = provider.sender().address!;

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    const keypairSafe = keypair;
    ui.write(`Pubkey: ${formatPublicKey(keypairSafe)}`);

    const code = await compile('WalletR');
    const jwalletCode = await compile('JettonWallet');
    const wallet = provider.open(WalletR.createFromConfig({ subwallet_id: 0, keypair: keypairSafe }, code));
    const firstWallet = wallet.address;
    ui.write(`0th WalletR address: ${firstWallet.toString()}`);
    ui.write(`will send to ${deployer}`);

    const last = Number(await ui.input('Last subwallet id:'));

    const api = provider.api();

    if (!(api instanceof TonClient)) {
        ui.write('no v2 api');
        return;
    }

    async function check(i: number) {
        ui.setActionPrompt(`Checking: ${i}/${last}`);
        const _walletR = WalletR.createFromConfig({ subwallet_id: i, keypair: keypairSafe }, code);
        const jwallet = provider.open(
            JettonWallet.createFromConfig(
                { owner: _walletR.address, master: minterAddress, code: jwalletCode },
                jwalletCode,
            ),
        );
        if (api instanceof TonClient) {
            try {
                const { state } = await api.getContractState(jwallet.address);
                if (state !== 'active') {
                    ui.write(`Non-active ${i}`);
                }
            } catch (e) {
                ui.write(`Non-active or failed ${i}: ${e}`);
            }
        }
    }

    for (let i = 0; i <= last; i++) {
        check(i);
        await sleep(10);
    }
}
