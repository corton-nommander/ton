import { Address, toNano } from '@ton/core';
import { compile, NetworkProvider, sleep } from '@ton/blueprint';
import { mnemonicNew, mnemonicToWalletKey } from '@ton/crypto';
import { ensureMnemonic, formatPublicKey, writeMnemonicFile } from './lib/mnemonic';
import { openWalletSpam } from './lib/wallets';
import { JettonMinter, jettonContentToCell } from '../wrappers/JettonMinter';

// Usage:
//   yarn blueprint run deployWalletSpam                         # interactive
//   COUNT=200 yarn blueprint run deployWalletSpam               # no EC
//   COUNT=200 EC=1000 yarn blueprint run deployWalletSpam       # with EC
//   COUNT=200 NEW=1 yarn blueprint run deployWalletSpam         # new mnemonic

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const envCount = process.env.COUNT;
    const envEc = process.env.EC;
    const envNew = process.env.NEW;

    let mnemonic: string[];
    if (envNew) {
        mnemonic = await mnemonicNew();
        await writeMnemonicFile(mnemonic);
        ui.write('Generated new mnemonic');
    } else {
        mnemonic = await ensureMnemonic(ui);
    }

    ui.write(`mnemonic: ${mnemonic.join(' ')}`);

    const keypair = await mnemonicToWalletKey(mnemonic);
    ui.write(`pubkey: ${formatPublicKey(keypair)}`);

    const code = await compile('WalletSpam');
    const wallet = await openWalletSpam(provider, keypair, 0, { code });
    ui.write('0th wallet: ' + wallet.address.toString());

    const count = envCount ? Number(envCount) : Number(await ui.input('Enter wallets count:'));

    let currencyAmount: bigint | undefined;
    if (envEc !== undefined) {
        currencyAmount = toNano(envEc);
        if (currencyAmount === 0n) currencyAmount = undefined;
    } else if (!envCount) {
        const ecInput = await ui.input('Enter deploy currency amount (0 to skip):');
        currencyAmount = toNano(ecInput);
        if (currencyAmount === 0n) currencyAmount = undefined;
    }

    // const admin = Address.parse('0:646f6f1f13124496504bd65441bc0d0677645a3b1e460896f0a299f13e53625a');

    // Deploy jetton minter if not yet deployed
    const minterCode = await compile('JettonMinter');
    const walletCode = await compile('JettonWallet');
    const content = jettonContentToCell({
        type: 1,
        uri: 'https://gist.githubusercontent.com/1IxI1/a49277da081f6e27811b6d5593730286/raw/12b434408399a1500f2f81a668f95a484b4b2f1b/pub4.json',
    });
    const admin = provider.sender().address!;
    const minter = provider.open(
        JettonMinter.createFromConfig({ admin, content, wallet_code: walletCode }, minterCode),
    );

    ui.write('Deploying jetton minter just in case: ' + minter.address.toString());
    await minter.sendDeploy(provider.sender(), toNano(10));
    await sleep(5000);
    ui.write('Minter deploy sent');

    await wallet.sendDeploy(provider.sender(), toNano(count * 1001), currencyAmount);
    ui.write('Sent deploy to 0th wallet ' + wallet.address.toString());

    // Custom waitForDeploy with retry on transient network errors (ECONNRESET etc.)
    for (let attempt = 1; attempt <= 30; attempt++) {
        try {
            if (await provider.isContractDeployed(wallet.address)) {
                break;
            }
        } catch (e: any) {
            if (e?.code === 'ECONNRESET' || e?.code === 'ETIMEDOUT' || e?.isAxiosError) {
                ui.write(`Awaiting deploy: transient error on attempt ${attempt}, retrying...`);
            } else {
                ui.write('Some error during waiting for deploy, nevermind');
            }
        }
        if (attempt === 30) {
            throw new Error('Contract was not deployed after 30 attempts');
        }
        await sleep(2000);
    }
    ui.write('Deployed, sending reproduce call');

    await wallet.sendReproduce(count, undefined, undefined, currencyAmount);
    ui.write('Sent reproduce call');
}
