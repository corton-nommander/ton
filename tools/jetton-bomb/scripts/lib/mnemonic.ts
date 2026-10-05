import { KeyPair, mnemonicNew, mnemonicToWalletKey } from '@ton/crypto';
import { UIProvider } from '@ton/blueprint';
import fss from 'fs';
import fs from 'fs/promises';

export const DEFAULT_MNEMONIC_PATH = 'mnemonic.txt';

export function formatPublicKey(keypair: KeyPair): string {
    return '0x' + keypair.publicKey.toString('hex');
}

export async function readMnemonicFile(path = DEFAULT_MNEMONIC_PATH): Promise<string[]> {
    const text = await fs.readFile(path, 'utf8');
    return text.trim().split(/\s+/);
}

export async function writeMnemonicFile(mnemonic: string[], path = DEFAULT_MNEMONIC_PATH) {
    await fs.writeFile(path, mnemonic.join(' '));
}

export async function ensureMnemonic(ui: UIProvider, path = DEFAULT_MNEMONIC_PATH): Promise<string[]> {
    let mnemonic: string[] | null = null;
    if (fss.existsSync(path)) {
        ui.write(`${path} already exists`);
        const ans = (await ui.input('Do you want to overwrite it? (y/n)')).trim().toLowerCase();
        if (ans !== 'y') {
            mnemonic = await readMnemonicFile(path);
        }
    }

    if (!mnemonic) {
        mnemonic = await mnemonicNew();
        await writeMnemonicFile(mnemonic, path);
    }

    return mnemonic;
}

export async function loadKeypairFromFile(ui: UIProvider, path = DEFAULT_MNEMONIC_PATH): Promise<KeyPair | null> {
    try {
        const mnemonic = await readMnemonicFile(path);
        return await mnemonicToWalletKey(mnemonic);
    } catch (e) {
        ui.write(`Failed to read ${path}. Create it first or set WALLET_MNEMONIC.`);
        return null;
    }
}

export async function loadKeypairFromEnv(ui: UIProvider, varName = 'WALLET_MNEMONIC'): Promise<KeyPair | null> {
    const mnemonic = process.env[varName];
    if (!mnemonic) {
        ui.write(`${varName} is not set`);
        return null;
    }
    return await mnemonicToWalletKey(mnemonic.trim().split(/\s+/));
}
