// Like jettonSpam.ts but talks to LiteServers directly via ton-lite-client.
// Skips toncenter/HTTP — round-robin over all liteservers in the global config.
//
// Usage:
//   COUNT=1000 FREQ=10 npx ts-node scripts/jettonSpamLs.ts
//   ONLY_EXT=1 COUNT=1000 FREQ=1000 npx ts-node scripts/jettonSpamLs.ts
//   MINTER_ADDRESS=EQ... COUNT=1000 PERIOD=200 npx ts-node scripts/jettonSpamLs.ts
//
// Env:
//   COUNT          required, number of subwallet ids to randomize over
//   FREQ           sends per second (mutually exclusive with PERIOD)
//   PERIOD         period in ms between fires
//   ONLY_EXT=1     send signed externals with no message refs (no jetton transfer)
//   MINTER_ADDRESS jetton minter address (required when ONLY_EXT is not set)
//   LS_CONFIG      path to TON global config (default ~/genesis/my.global.config.json)

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { Address, Cell, OpenedContract, SendMode, beginCell, toNano } from '@ton/core';
import { mnemonicToWalletKey } from '@ton/crypto';
import { LiteClient, LiteRoundRobinEngine, LiteSingleEngine } from 'ton-lite-client';
import { randomAddress } from '@ton/test-utils';
import { WalletSpam, spamMsg } from '../wrappers/WalletSpam';
import { JettonWallet } from '../wrappers/JettonWallet';
import { readMnemonicFile } from './lib/mnemonic';

interface LiteServer {
    ip: number;
    port: number;
    id: { '@type': string; key: string };
}

interface GlobalConfig {
    liteservers: LiteServer[];
}

function intToIp(ip: number): string {
    if (ip < 0) ip += 0x100000000;
    return [(ip >>> 24) & 0xff, (ip >>> 16) & 0xff, (ip >>> 8) & 0xff, ip & 0xff].join('.');
}

function loadCompiledCell(name: string): Cell {
    const json = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'build', `${name}.compiled.json`), 'utf8'));
    return Cell.fromBoc(Buffer.from(json.hex, 'hex'))[0];
}

async function main() {
    const envCount = process.env.COUNT;
    const envFreq = process.env.FREQ;
    const envPeriod = process.env.PERIOD;
    const onlyExt = process.env.ONLY_EXT === '1';
    const minterEnv = process.env.MINTER_ADDRESS;
    const configPath = process.env.LS_CONFIG || path.join(os.homedir(), 'genesis', 'my.global.config.json');

    if (!envCount) {
        console.error('COUNT is required');
        process.exit(1);
    }
    if (envFreq && envPeriod) {
        console.error('FREQ and PERIOD are mutually exclusive');
        process.exit(1);
    }

    const count = Number(envCount);
    const targetRate = envFreq ? Number(envFreq) : 1000 / Number(envPeriod || 100);

    const mnemonic = await readMnemonicFile();
    const keypair = await mnemonicToWalletKey(mnemonic);
    console.log(`Pubkey: 0x${keypair.publicKey.toString('hex')}`);

    if (!fs.existsSync(configPath)) {
        console.error(`Liteservers config not found at ${configPath}`);
        process.exit(1);
    }
    const config = JSON.parse(fs.readFileSync(configPath, 'utf8')) as GlobalConfig;
    if (!config.liteservers?.length) {
        console.error(`No liteservers in ${configPath}`);
        process.exit(1);
    }
    console.log(`Loaded ${config.liteservers.length} liteservers from ${configPath}`);

    const engines = config.liteservers.map(
        (ls) =>
            new LiteSingleEngine({
                host: `tcp://${intToIp(ls.ip)}:${ls.port}`,
                publicKey: Buffer.from(ls.id.key, 'base64'),
            }),
    );
    const engine = new LiteRoundRobinEngine(engines);
    const client = new LiteClient({ engine });

    const code = loadCompiledCell('WalletSpam');

    let walletCode: Cell | undefined;
    let minterAddr: Address | undefined;
    const isOptimized = true;

    if (!onlyExt) {
        if (!minterEnv) {
            console.error('MINTER_ADDRESS is required when ONLY_EXT is not set');
            process.exit(1);
        }
        walletCode = loadCompiledCell('JettonWallet');
        minterAddr = Address.parse(minterEnv);
        console.log(`Minter: ${minterAddr.toString()}`);
    } else {
        console.log('ONLY_EXT mode: sending signed externals with no message refs');
    }

    console.log(`Count: ${count}, Target rate: ${targetRate.toFixed(1)} req/s`);

    let sent = 0;
    let failed = 0;

    const wallets = new Map<number, OpenedContract<WalletSpam>>();
    const seqnos = new Map<number, number>();

    function getWallet(id: number): OpenedContract<WalletSpam> {
        let w = wallets.get(id);
        if (!w) {
            w = client.open(WalletSpam.createFromConfig({ subwallet_id: id, keypair, spam_msg: spamMsg }, code));
            wallets.set(id, w);
        }
        return w;
    }

    async function sendFrom(id: number) {
        const wallet = getWallet(id);

        let seqno = seqnos.get(id);
        if (seqno === undefined) {
            seqno = await wallet.getSeqno();
        }
        seqnos.set(id, seqno + 1); // optimistic bump before send

        try {
            if (onlyExt) {
                const transfer = wallet.createTransfer({ messages: [], seqno });
                await wallet.sendSignExternal(transfer);
                return;
            }

            const dest = randomAddress();
            const comment = Math.floor(Math.random() * 1000000).toString();

            const transferMsg = JettonWallet.transferMessage(
                1n,
                dest,
                wallet.address,
                Cell.EMPTY,
                toNano('0.05'),
                beginCell().storeUint(0, 32).storeStringTail(comment).endCell(),
            );

            const jwallet = JettonWallet.createFromConfig(
                { owner: wallet.address, master: minterAddr!, code: walletCode!, optimized: isOptimized },
                walletCode!,
            );

            await wallet.send(
                {
                    to: jwallet.address,
                    value: toNano('0.16'),
                    bounce: true,
                    sendMode: SendMode.PAY_GAS_SEPARATELY,
                    body: transferMsg,
                    init: jwallet.init,
                },
                seqno,
            );
        } catch (e) {
            seqnos.delete(id); // unknown state — refetch next time
            throw e;
        }
    }

    console.log('Starting spam loop (token-bucket via setImmediate)...');

    // Token bucket: owed increments by targetRate * elapsed seconds.
    // setImmediate gives sub-ms granularity, hrtime.bigint() gives ns precision.
    // Cap owed at 1 second of credit so a stall doesn't flood-burst on resume.
    const maxOwed = targetRate;
    let owed = 0;
    let last = process.hrtime.bigint();
    let lastReportAt = sent + failed;

    function tick() {
        const now = process.hrtime.bigint();
        const dtSec = Number(now - last) / 1e9;
        last = now;
        owed += dtSec * targetRate;
        if (owed > maxOwed) owed = maxOwed;

        while (owed >= 1) {
            owed -= 1;
            const id = Math.floor(Math.random() * count);
            sendFrom(id)
                .then(() => {
                    sent++;
                })
                .catch(() => {
                    failed++;
                });
        }

        const total = sent + failed;
        if (total >= lastReportAt + 1000) {
            lastReportAt = total - (total % 1000);
            console.log(`sent: ${sent}, failed: ${failed}`);
        }

        setImmediate(tick);
    }
    setImmediate(tick);
}

main().catch((e) => {
    console.error(e);
    process.exit(1);
});
