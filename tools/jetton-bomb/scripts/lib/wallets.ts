import { compile, NetworkProvider } from '@ton/blueprint';
import { Cell } from '@ton/core';
import { KeyPair } from '@ton/crypto';
import { WalletSpam, spamMsg as defaultSpamMsg } from '../../wrappers/WalletSpam';
import { WalletR } from '../../wrappers/WalletR';

export async function openWalletSpam(
    provider: NetworkProvider,
    keypair: KeyPair,
    subwalletId: number,
    options: { code?: Cell; spamMsg?: Cell } = {},
) {
    const code = options.code ?? (await compile('WalletSpam'));
    const spamMsg = options.spamMsg ?? defaultSpamMsg;
    return provider.open(WalletSpam.createFromConfig({ subwallet_id: subwalletId, keypair, spam_msg: spamMsg }, code));
}

export async function openWalletR(provider: NetworkProvider, keypair: KeyPair, subwalletId: number, code?: Cell) {
    const walletCode = code ?? (await compile('WalletR'));
    return provider.open(WalletR.createFromConfig({ subwallet_id: subwalletId, keypair }, walletCode));
}
