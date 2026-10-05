import { Address, Cell, OpenedContract, SendMode, beginCell, internal, toNano } from '@ton/core';
import { KeyPair, mnemonicToPrivateKey, mnemonicToWalletKey } from '@ton/crypto';
import { TonClient, WalletContractV5R1 } from '@ton/ton';
import { LiteClient, LiteRoundRobinEngine, LiteSingleEngine } from 'ton-lite-client';
import { Context, Telegraf } from 'telegraf';
import { config as blueprintConfig } from '../blueprint.config';
import { Opcodes as TpsOpcodes } from '../wrappers/StaticTps';
import { WalletSpam, spamMsg } from '../wrappers/WalletSpam';
import { JettonMinter } from '../wrappers/JettonMinter';
import { JettonWallet } from '../wrappers/JettonWallet';
import { MINTERS } from './lib/addresses';
import { readMnemonicFile } from './lib/mnemonic';
import { randomAddress } from '@ton/test-utils';
import { randomBytes } from 'crypto';
import * as fs from 'fs';
import * as path from 'path';

const LOG_FILE = path.join(__dirname, '..', 'bot.log');
const SHARDS_PATH = path.join(__dirname, '..', 'tps_shards.json');
const RUN_AMOUNT_PER_SHARD_TON = '1000';
const STOP_FEE_PER_SHARD_TON = '0.05';
const WALLET_VERSION = 'v5r1';

const SPAM_WALLET_COUNT = 100000;
const SPAM_MINTER = MINTERS.pub4;
const SPAM_IS_OPTIMIZED = true;

type Client = TonClient | LiteClient;

type Backend =
    | { kind: 'http'; endpoint: string; apiKey?: string }
    | { kind: 'lite'; liteservers: number };

type Runtime = {
    wallet: OpenedContract<WalletContractV5R1>;
    secretKey: Buffer;
    walletAddress: Address;
    shardAddresses: Address[];
    client: Client;
    backend: Backend;
};

type SpamState = {
    running: boolean;
    abort: boolean;
    sent: number;
    failed: number;
};

function requireEnv(name: string): string {
    const value = process.env[name]?.trim();
    if (!value) {
        throw new Error(`${name} is not set`);
    }
    return value;
}

function parsePositiveInt(value: string): number | null {
    const parsed = Number(value);
    if (!Number.isInteger(parsed) || parsed <= 0) {
        return null;
    }
    return parsed;
}

function getMessageText(ctx: Context): string {
    const message = ctx.message;
    if (!message || !('text' in message)) {
        return '';
    }
    return message.text;
}

function userTag(ctx: Context): string {
    const from = ctx.from;
    if (!from) return 'unknown';
    return `${from.username ?? from.first_name}(${from.id})`;
}

function loadWalletSpamCode(): Cell {
    const compiled = JSON.parse(
        fs.readFileSync(path.join(__dirname, '..', 'build', 'WalletSpam.compiled.json'), 'utf-8'),
    );
    const actualcode = compiled.hex;
    const oldcode =
        'b5ee9c7241020c010001bb000114ff00f4a413f4bcf2c80b01020120020b020148030601e2d03221c700915be0d0d3030171b0915be001d31f018210ba5e5d50bd915be0d31fd31f3002fa4030ed44d0d31f31d31fd3ffd4302070fb005324be925f06e054351570552003c8cb1f12cb1fcbffccc970f82a21c8cb01f40012f400cb00c9f9008309c8cb0acbffc9d05003c705f2e19104015281ff77f833206e913099d0d70b0fc001f2d29ae2708212540be40072fb025331a17f6d91218ae85f080500fc705354a0a4537903c8cb1f12cb1fcbffccc970f82a21c8cb01f40012f400cb00c920f9008309c8cb0acbffc9d0708212540be400712509a4208100c8be5319beb19d375f033570287083066d090407de8210ba5e5d50c8cb1f14cb1f52a0cb1fc9c8801801cb055005cf1658fa0218f4005a775003cb6accccc95005fb00020120070a02012008090017bb39ced44d0d33f31d70bff80011b8c97ed44d0d70b1f80017bfdddf6a268698f98eb858fc00a2f28308d71820d31fd31fd31f02f823bbf263ed44d0d31fd31fd3ffd4305143baf2a15151baf2a205f901541064f910f2a3f800019320d74a96d307d402fb00e83003a40303c8cb1f12cb1fcbffccc9ed5496ba1903';
    return Cell.fromBoc(Buffer.from(oldcode, 'hex'))[0];
}

function resolveToncenterConfig(): { endpoint: string; apiKey?: string } {
    const fromConfig =
        typeof blueprintConfig.network === 'object' && blueprintConfig.network !== null
            ? blueprintConfig.network
            : null;

    const endpoint = process.env.TONCENTER_ENDPOINT?.trim() || fromConfig?.endpoint || '';
    if (!endpoint) {
        throw new Error(
            'TON endpoint is not configured. Set TONCENTER_ENDPOINT or network.endpoint in blueprint.config.ts',
        );
    }

    const apiKey = process.env.TONCENTER_API_KEY?.trim() || fromConfig?.key || undefined;
    return { endpoint, apiKey };
}

function intToIP(int: number): string {
    const part1 = int & 255;
    const part2 = (int >> 8) & 255;
    const part3 = (int >> 16) & 255;
    const part4 = (int >> 24) & 255;
    return `${(part4 + 256) % 256}.${(part3 + 256) % 256}.${(part2 + 256) % 256}.${(part1 + 256) % 256}`;
}

type LiteserverConfig = {
    liteservers: Array<{ ip: number; port: number; id: { key: string } }>;
};

function buildLiteClientFromJson(data: unknown): { client: LiteClient; count: number } {
    if (!data || typeof data !== 'object' || !Array.isArray((data as LiteserverConfig).liteservers)) {
        throw new Error('Invalid liteserver config: missing "liteservers" array');
    }
    const servers = (data as LiteserverConfig).liteservers;
    if (servers.length === 0) {
        throw new Error('Invalid liteserver config: "liteservers" is empty');
    }
    const engines = servers.map((server, i) => {
        if (
            typeof server?.ip !== 'number' ||
            typeof server?.port !== 'number' ||
            typeof server?.id !== 'object' ||
            typeof server?.id?.key !== 'string'
        ) {
            throw new Error(`Invalid liteserver entry #${i}`);
        }
        return new LiteSingleEngine({
            host: `tcp://${intToIP(server.ip)}:${server.port}`,
            publicKey: Buffer.from(server.id.key, 'base64'),
        });
    });
    const engine = new LiteRoundRobinEngine(engines);
    return { client: new LiteClient({ engine }), count: servers.length };
}

function buildRuntime(
    client: Client,
    backend: Backend,
    keyPair: KeyPair,
    shardAddresses: Address[],
): Runtime {
    const wallet = client.open(
        WalletContractV5R1.create({
            workchain: 0,
            publicKey: keyPair.publicKey,
        }),
    );

    return {
        wallet,
        secretKey: keyPair.secretKey,
        walletAddress: wallet.address,
        shardAddresses,
        client,
        backend,
    };
}

function loadShardAddresses(): Address[] {
    if (!fs.existsSync(SHARDS_PATH)) {
        throw new Error(`tps_shards.json not found at ${SHARDS_PATH}; run deployTPSShards first`);
    }
    const data = JSON.parse(fs.readFileSync(SHARDS_PATH, 'utf-8'));
    if (!Array.isArray(data.shards) || data.shards.length !== 16) {
        throw new Error(`expected 16 shards in ${SHARDS_PATH}, got ${data.shards?.length}`);
    }
    return [...data.shards]
        .sort((a: { shard: number }, b: { shard: number }) => a.shard - b.shard)
        .map((s: { address: string }) => Address.parse(s.address));
}

function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

const logStream = fs.createWriteStream(LOG_FILE, { flags: 'a' });

function extractErrorDetails(e: unknown): { error: string; response?: unknown } {
    if (e && typeof e === 'object' && 'response' in e) {
        const axiosErr = e as { message?: string; response?: { status?: number; data?: unknown } };
        return {
            error: axiosErr.message ?? String(e),
            response: axiosErr.response?.data,
        };
    }
    return { error: e instanceof Error ? e.message : String(e) };
}

function log(level: string, msg: string, data?: unknown) {
    const ts = new Date().toISOString();
    const line = data ? `${ts} [${level}] ${msg} ${JSON.stringify(data)}` : `${ts} [${level}] ${msg}`;
    logStream.write(line + '\n');
    if (level === 'ERROR') console.error(line);
}

async function runSpamLoop(
    getRuntime: () => Runtime,
    spam: SpamState,
    periodMs: number,
    freq: number,
    ctx: Context,
) {
    log('INFO', `spam loop init: reading mnemonic`);
    const spamMnemonic = await readMnemonicFile();
    const spamKeypair = await mnemonicToWalletKey(spamMnemonic);
    log('INFO', `spam loop init: mnemonic loaded`, { pubkey: '0x' + spamKeypair.publicKey.toString('hex') });

    const code = loadWalletSpamCode();
    log('INFO', `spam loop init: contract code loaded`);

    const jminter = getRuntime().client.open(JettonMinter.createFromAddress(SPAM_MINTER));
    log('INFO', `spam loop init: fetching minter data`, { minter: SPAM_MINTER.toString() });
    const jwalletCode = (await jminter.getJettonData()).walletCode;
    log('INFO', `spam loop init: minter data loaded, precomputing pool`, { wallets: SPAM_WALLET_COUNT });

    type PoolEntry = {
        walletConfig: { subwallet_id: number; keypair: typeof spamKeypair; spam_msg: typeof spamMsg };
        walletAddress: Address;
        jwalletAddress: Address;
        jwalletInit: NonNullable<ReturnType<typeof JettonWallet.createFromConfig>['init']>;
    };

    const poolStart = Date.now();
    const pool: PoolEntry[] = new Array(SPAM_WALLET_COUNT);
    for (let id = 0; id < SPAM_WALLET_COUNT; id++) {
        const walletConfig = { subwallet_id: id, keypair: spamKeypair, spam_msg: spamMsg };
        const w = WalletSpam.createFromConfig(walletConfig, code);
        const j = JettonWallet.createFromConfig(
            { owner: w.address, master: SPAM_MINTER, code: jwalletCode, optimized: SPAM_IS_OPTIMIZED },
            jwalletCode,
        );
        pool[id] = {
            walletConfig,
            walletAddress: w.address,
            jwalletAddress: j.address,
            jwalletInit: j.init!,
        };
    }
    log('INFO', `spam loop init: pool ready, starting loop`, { durationMs: Date.now() - poolStart });

    // Timing accumulators (reset each stats window)
    let syncSumMs = 0;
    let syncMax = 0;
    let driftSumMs = 0;
    let driftMax = 0;
    let ticks = 0;

    while (!spam.abort) {
        const tickStart = Date.now();
        const client = getRuntime().client;
        const entry = pool[Math.floor(Math.random() * pool.length)];
        const wallet = client.open(WalletSpam.createFromAddress(entry.walletAddress, entry.walletConfig));
        const dest = randomAddress();
        const comment = Math.floor(Math.random() * 1000000).toString();

        const transferMsg = JettonWallet.transferMessage(
            1n,
            dest,
            entry.walletAddress,
            Cell.EMPTY,
            toNano('0.05'),
            beginCell().storeUint(0, 32).storeStringTail(comment).endCell(),
        );

        wallet
            .send({
                to: entry.jwalletAddress,
                value: toNano('0.16'),
                bounce: true,
                sendMode: SendMode.PAY_GAS_SEPARATELY,
                body: transferMsg,
                init: entry.jwalletInit,
            })
            .then(() => {
                spam.sent++;
                log('DEBUG', `spam sent`, { id: entry.walletConfig.subwallet_id, dest: dest.toString() });
            })
            .catch((e) => {
                spam.failed++;
                log('ERROR', `spam send failed`, {
                    id: entry.walletConfig.subwallet_id,
                    wallet: entry.walletAddress.toString(),
                    ...extractErrorDetails(e),
                });
            });

        const syncMs = Date.now() - tickStart;
        syncSumMs += syncMs;
        if (syncMs > syncMax) syncMax = syncMs;
        ticks++;

        const statsEvery = freq * 30; // every ~30s
        if ((spam.sent + spam.failed) % statsEvery === 0 && spam.sent + spam.failed > 0) {
            const avgSync = ticks ? (syncSumMs / ticks).toFixed(2) : '0';
            const avgDrift = ticks ? (driftSumMs / ticks).toFixed(2) : '0';
            log('INFO', `spam stats`, {
                sent: spam.sent,
                failed: spam.failed,
                total: spam.sent + spam.failed,
                avgSyncMs: avgSync,
                maxSyncMs: syncMax,
                avgDriftMs: avgDrift,
                maxDriftMs: driftMax,
                ticks,
            });
            ctx.reply(
                `spam stats: sent=${spam.sent}, failed=${spam.failed}, sync avg=${avgSync}ms max=${syncMax}ms, drift avg=${avgDrift}ms max=${driftMax}ms`,
            ).catch(() => {});
            syncSumMs = 0;
            syncMax = 0;
            driftSumMs = 0;
            driftMax = 0;
            ticks = 0;
        }

        const sleepStart = Date.now();
        await sleep(periodMs);
        const drift = Date.now() - sleepStart - periodMs;
        if (drift > 0) driftSumMs += drift;
        if (drift > driftMax) driftMax = drift;
    }

    log('INFO', `spam loop finished`, { sent: spam.sent, failed: spam.failed });
    spam.running = false;
}

// Builds an external-message body of ~40 cells / ~20000 bits, padded with random
// bytes so each external hashes uniquely (no dedup at the liteserver/validator).
// The wallet will likely abort post-accept_message (random refs aren't valid msgs),
// but the full payload still travels over the network — that's the point.
function buildHugeExternalMsg(subwalletId: number, seqno: number, until: number): Cell {
    const TOTAL_CELLS = 40;
    const CHILD_BYTES = 64; // 512 bits per child; 39 * 512 + 96 header = 20064 bits
    let inner: Cell | null = null;
    for (let i = 0; i < TOTAL_CELLS - 1; i++) {
        const b = beginCell().storeBuffer(randomBytes(CHILD_BYTES));
        if (inner) b.storeRef(inner);
        inner = b.endCell();
    }
    return beginCell()
        .storeUint(subwalletId, 32)
        .storeUint(until, 32)
        .storeUint(seqno, 32)
        .storeRef(inner!)
        .endCell();
}

async function runExternalSpamLoop(
    getRuntime: () => Runtime,
    spam: SpamState,
    periodMs: number,
    freq: number,
    ctx: Context,
    huge: boolean,
) {
    log('INFO', `external loop init: reading mnemonic`);
    const spamMnemonic = await readMnemonicFile();
    const spamKeypair = await mnemonicToWalletKey(spamMnemonic);
    log('INFO', `external loop init: mnemonic loaded`, { pubkey: '0x' + spamKeypair.publicKey.toString('hex') });

    const code = loadWalletSpamCode();
    log('INFO', `external loop init: contract code loaded`);

    type ExternalPoolEntry = {
        walletConfig: { subwallet_id: number; keypair: KeyPair; spam_msg: Cell };
        walletAddress: Address;
    };

    const poolStart = Date.now();
    const pool: ExternalPoolEntry[] = new Array(SPAM_WALLET_COUNT);
    for (let id = 0; id < SPAM_WALLET_COUNT; id++) {
        const walletConfig = { subwallet_id: id, keypair: spamKeypair, spam_msg: spamMsg };
        const w = WalletSpam.createFromConfig(walletConfig, code);
        pool[id] = {
            walletConfig,
            walletAddress: w.address,
        };
    }
    log('INFO', `external loop init: pool ready, starting loop`, { durationMs: Date.now() - poolStart });

    let syncSumMs = 0;
    let syncMax = 0;
    let driftSumMs = 0;
    let driftMax = 0;
    let ticks = 0;

    const sendOne = async (entry: ExternalPoolEntry, client: Client) => {
        const wallet = client.open(WalletSpam.createFromAddress(entry.walletAddress, entry.walletConfig));
        const seqno = await wallet.getSeqno();
        const transfer = huge
            ? buildHugeExternalMsg(entry.walletConfig.subwallet_id, seqno, Math.floor(Date.now() / 1000) + 40)
            : wallet.createTransfer({ messages: [], seqno });
        await wallet.sendSignExternal(transfer);
    };

    while (!spam.abort) {
        const tickStart = Date.now();
        const client = getRuntime().client;
        const entry = pool[Math.floor(Math.random() * pool.length)];

        sendOne(entry, client)
            .then(() => {
                spam.sent++;
                log('DEBUG', `external sent`, { id: entry.walletConfig.subwallet_id });
            })
            .catch((e) => {
                spam.failed++;
                log('ERROR', `external send failed`, {
                    id: entry.walletConfig.subwallet_id,
                    wallet: entry.walletAddress.toString(),
                    ...extractErrorDetails(e),
                });
            });

        const syncMs = Date.now() - tickStart;
        syncSumMs += syncMs;
        if (syncMs > syncMax) syncMax = syncMs;
        ticks++;

        const statsEvery = freq * 30; // every ~30s
        if ((spam.sent + spam.failed) % statsEvery === 0 && spam.sent + spam.failed > 0) {
            const avgSync = ticks ? (syncSumMs / ticks).toFixed(2) : '0';
            const avgDrift = ticks ? (driftSumMs / ticks).toFixed(2) : '0';
            log('INFO', `external stats`, {
                sent: spam.sent,
                failed: spam.failed,
                total: spam.sent + spam.failed,
                avgSyncMs: avgSync,
                maxSyncMs: syncMax,
                avgDriftMs: avgDrift,
                maxDriftMs: driftMax,
                ticks,
            });
            ctx.reply(
                `external stats: sent=${spam.sent}, failed=${spam.failed}, sync avg=${avgSync}ms max=${syncMax}ms, drift avg=${avgDrift}ms max=${driftMax}ms`,
            ).catch(() => {});
            syncSumMs = 0;
            syncMax = 0;
            driftSumMs = 0;
            driftMax = 0;
            ticks = 0;
        }

        const sleepStart = Date.now();
        await sleep(periodMs);
        const drift = Date.now() - sleepStart - periodMs;
        if (drift > 0) driftSumMs += drift;
        if (drift > driftMax) driftMax = drift;
    }

    log('INFO', `external loop finished`, { sent: spam.sent, failed: spam.failed });
    spam.running = false;
}

async function main() {
    const token = requireEnv('TELEGRAM_BOT_TOKEN');
    const mnemonic = requireEnv('WALLET_MNEMONIC');
    const keyPair = await mnemonicToPrivateKey(mnemonic.split(/\s+/));

    const shardAddresses = loadShardAddresses();
    log('INFO', `loaded shard addresses`, { count: shardAddresses.length });

    const { endpoint, apiKey } = resolveToncenterConfig();
    const defaultHttpBackend: Backend = { kind: 'http', endpoint, apiKey };
    log('INFO', `creating runtime`, { endpoint, hasApiKey: !!apiKey });
    let runtime: Runtime = buildRuntime(
        new TonClient({ endpoint, apiKey }),
        defaultHttpBackend,
        keyPair,
        shardAddresses,
    );

    const switchToHttp = (backend: Extract<Backend, { kind: 'http' }>): Runtime => {
        return buildRuntime(
            new TonClient({ endpoint: backend.endpoint, apiKey: backend.apiKey }),
            backend,
            keyPair,
            shardAddresses,
        );
    };

    const pendingConfig = new Set<number>();
    const spam: SpamState = { running: false, abort: false, sent: 0, failed: 0 };
    const externalSpam: SpamState = { running: false, abort: false, sent: 0, failed: 0 };

    const bot = new Telegraf(token);
    log('INFO', `bot started`, { wallet: runtime.walletAddress.toString(), version: WALLET_VERSION });
    console.log(
        `telegramTpsBot launched. wallet=${runtime.walletAddress.toString()} version=${WALLET_VERSION} log=${LOG_FILE}`,
    );

    bot.use(async (ctx, next) => {
        const userId = ctx.from?.id;
        if (!userId || !pendingConfig.has(userId)) return next();

        const msg = ctx.message;
        if (msg && 'document' in msg && msg.document) {
            pendingConfig.delete(userId);
            const user = userTag(ctx);
            const fileName = msg.document.file_name ?? '(unnamed)';
            log('INFO', `config upload received`, { user, fileName, size: msg.document.file_size });
            try {
                const link = await ctx.telegram.getFileLink(msg.document.file_id);
                const res = await fetch(link.toString());
                if (!res.ok) throw new Error(`download failed: ${res.status}`);
                const data = await res.json();
                const { client, count } = buildLiteClientFromJson(data);
                const prev = runtime.backend.kind;
                runtime = buildRuntime(client, { kind: 'lite', liteservers: count }, keyPair, shardAddresses);
                log('INFO', `backend switch`, { user, from: prev, to: 'lite', liteservers: count });
                await ctx.reply(`Switched to liteserver backend (${count} servers)`);
            } catch (e) {
                log('ERROR', `backend switch failed`, { user, ...extractErrorDetails(e) });
                await ctx.reply(`Config load failed: ${extractErrorDetails(e).error}`);
            }
            return;
        }

        pendingConfig.delete(userId);
        await ctx.reply('Config upload cancelled');
        return next();
    });

    bot.command('start', async (ctx) => {
        const user = userTag(ctx);
        const text = getMessageText(ctx);
        const parts = text.trim().split(/\s+/);
        const tpsArg = parts[1];
        log('INFO', `/start command`, { user, text });

        if (!tpsArg) {
            await ctx.reply('Usage: /start <tps>');
            return;
        }

        const tps = parsePositiveInt(tpsArg);
        if (tps === null) {
            log('WARN', `/start invalid tps arg`, { user, tpsArg });
            await ctx.reply('TPS must be a positive integer. Usage: /start <tps>');
            return;
        }

        const target = runtime.shardAddresses[0];
        const value = toNano(RUN_AMOUNT_PER_SHARD_TON);
        const body = beginCell().storeUint(TpsOpcodes.run, 32).storeUint(tps, 32).endCell();
        const messages = [internal({ to: target, value, bounce: true, body })];

        await ctx.reply(
            `Running TPS on 1 shard\nTarget: ${target.toString()}\nTarget TPS: ${tps}\nTON: ${RUN_AMOUNT_PER_SHARD_TON}`,
        );
        try {
            const seqno = await runtime.wallet.getSeqno();
            await runtime.wallet.sendTransfer({
                seqno,
                secretKey: runtime.secretKey,
                messages,
                sendMode: SendMode.PAY_GAS_SEPARATELY,
            });
            log('INFO', `TPS run sent`, { user, tps, target: target.toString(), seqno });
            await ctx.reply('run sent');
        } catch (error) {
            const details = extractErrorDetails(error);
            log('ERROR', `TPS run failed`, { user, tps, ...details });
            await ctx.reply(`run failed: ${details.error}`);
        }
    });

    bot.command('stop', async (ctx) => {
        const user = userTag(ctx);
        log('INFO', `/stop command`, { user });

        const target = runtime.shardAddresses[0];
        await ctx.reply(`Stopping TPS\nTarget: ${target.toString()}`);

        const value = toNano(STOP_FEE_PER_SHARD_TON);
        const body = beginCell().storeUint(TpsOpcodes.stop, 32).endCell();
        const messages = [internal({ to: target, value, bounce: true, body })];

        try {
            const seqno = await runtime.wallet.getSeqno();
            await runtime.wallet.sendTransfer({
                seqno,
                secretKey: runtime.secretKey,
                messages,
                sendMode: SendMode.PAY_GAS_SEPARATELY,
            });
            log('INFO', `TPS stop sent`, { user, target: target.toString(), seqno });
            await ctx.reply('stop sent');
        } catch (error) {
            const details = extractErrorDetails(error);
            log('ERROR', `TPS stop failed`, { user, ...details });
            await ctx.reply(`stop failed: ${details.error}`);
        }
    });

    bot.command('shards', async (ctx) => {
        const user = userTag(ctx);
        const text = getMessageText(ctx);
        const parts = text.trim().split(/\s+/);
        const tpsArg = parts[1];
        log('INFO', `/shards command`, { user, text });

        if (!tpsArg) {
            await ctx.reply('Usage: /shards <tps>  (TPS is per contract; spam runs on all 16 shards)');
            return;
        }

        const tps = parsePositiveInt(tpsArg);
        if (tps === null) {
            log('WARN', `/shards invalid tps arg`, { user, tpsArg });
            await ctx.reply('TPS must be a positive integer. Usage: /shards <tps>');
            return;
        }

        const valuePerShard = toNano(RUN_AMOUNT_PER_SHARD_TON);
        const body = beginCell().storeUint(TpsOpcodes.run, 32).storeUint(tps, 32).endCell();
        const messages = runtime.shardAddresses.map((to) =>
            internal({ to, value: valuePerShard, bounce: true, body }),
        );

        const totalTps = tps * runtime.shardAddresses.length;
        await ctx.reply(
            `Running TPS on ${runtime.shardAddresses.length} shards\n` +
                `Target TPS per contract: ${tps} (total ~${totalTps})\n` +
                `TON per contract: ${RUN_AMOUNT_PER_SHARD_TON} (total ${Number(RUN_AMOUNT_PER_SHARD_TON) * runtime.shardAddresses.length})`,
        );
        try {
            const seqno = await runtime.wallet.getSeqno();
            await runtime.wallet.sendTransfer({
                seqno,
                secretKey: runtime.secretKey,
                messages,
                sendMode: SendMode.PAY_GAS_SEPARATELY,
            });
            log('INFO', `shards run sent`, {
                user,
                tps,
                shards: runtime.shardAddresses.length,
                amountPerShard: RUN_AMOUNT_PER_SHARD_TON,
                seqno,
            });
            await ctx.reply('run sent');
        } catch (error) {
            const details = extractErrorDetails(error);
            log('ERROR', `shards run failed`, { user, tps, ...details });
            await ctx.reply(`run failed: ${details.error}`);
        }
    });

    bot.command('shards_stop', async (ctx) => {
        const user = userTag(ctx);
        log('INFO', `/shards_stop command`, { user });
        await ctx.reply(`Stopping TPS on ${runtime.shardAddresses.length} shards`);

        const valuePerShard = toNano(STOP_FEE_PER_SHARD_TON);
        const body = beginCell().storeUint(TpsOpcodes.stop, 32).endCell();
        const messages = runtime.shardAddresses.map((to) =>
            internal({ to, value: valuePerShard, bounce: true, body }),
        );

        try {
            const seqno = await runtime.wallet.getSeqno();
            await runtime.wallet.sendTransfer({
                seqno,
                secretKey: runtime.secretKey,
                messages,
                sendMode: SendMode.PAY_GAS_SEPARATELY,
            });
            log('INFO', `shards stop sent`, { user, shards: runtime.shardAddresses.length, seqno });
            await ctx.reply('stop sent');
        } catch (error) {
            const details = extractErrorDetails(error);
            log('ERROR', `shards stop failed`, { user, ...details });
            await ctx.reply(`stop failed: ${details.error}`);
        }
    });

    bot.command('config', async (ctx) => {
        const user = userTag(ctx);
        log('INFO', `/config command`, { user });
        pendingConfig.add(ctx.from!.id);
        await ctx.reply('Send the liteserver config .json file. Any non-document reply cancels.');
    });

    bot.command('api', async (ctx) => {
        const user = userTag(ctx);
        const text = getMessageText(ctx);
        const parts = text.trim().split(/\s+/);
        const url = parts[1];
        const key = parts[2];
        log('INFO', `/api command`, { user, url: url ?? '(default)', hasKey: !!key });

        const prev = runtime.backend.kind;
        try {
            if (!url) {
                runtime = switchToHttp(defaultHttpBackend);
                log('INFO', `backend switch`, {
                    user,
                    from: prev,
                    to: 'http',
                    endpoint: defaultHttpBackend.endpoint,
                    hasApiKey: !!defaultHttpBackend.apiKey,
                    default: true,
                });
                await ctx.reply(`Reverted to default HTTP endpoint: ${defaultHttpBackend.endpoint}`);
            } else {
                runtime = switchToHttp({ kind: 'http', endpoint: url, apiKey: key });
                log('INFO', `backend switch`, { user, from: prev, to: 'http', endpoint: url, hasApiKey: !!key });
                await ctx.reply(`Switched to HTTP backend: ${url}${key ? ' (with key)' : ''}`);
            }
        } catch (e) {
            log('ERROR', `backend switch failed`, { user, ...extractErrorDetails(e) });
            await ctx.reply(`API switch failed: ${extractErrorDetails(e).error}`);
        }
    });

    bot.command('jetton', async (ctx) => {
        const user = userTag(ctx);
        const text = getMessageText(ctx);
        log('INFO', `/jetton command`, { user, text });

        if (spam.running) {
            log('WARN', `jetton spam already running`, { user, sent: spam.sent, failed: spam.failed });
            await ctx.reply(`Already running. sent=${spam.sent}, failed=${spam.failed}`);
            return;
        }

        const parts = text.trim().split(/\s+/);
        const freqArg = parts[1];

        if (!freqArg) {
            await ctx.reply('Usage: /jetton <freq>');
            return;
        }

        const freq = parsePositiveInt(freqArg);
        if (freq === null) {
            log('WARN', `/jetton invalid freq arg`, { user, freqArg });
            await ctx.reply('freq must be a positive integer. Usage: /jetton <freq>');
            return;
        }

        const periodMs = Math.floor(1000 / freq);
        spam.running = true;
        spam.abort = false;
        spam.sent = 0;
        spam.failed = 0;

        log('INFO', `jetton spam starting`, { user, freq, periodMs, wallets: SPAM_WALLET_COUNT });
        await ctx.reply(`Starting jetton spam\nWallets: ${SPAM_WALLET_COUNT}\nFreq: ${freq} req/s (${periodMs}ms)`);

        runSpamLoop(() => runtime, spam, periodMs, freq, ctx).catch((e) => {
            const details = extractErrorDetails(e);
            log('ERROR', `spam loop crashed`, details);
            ctx.reply(`spam loop crashed: ${details.error}`).catch(() => {});
            spam.running = false;
        });
    });

    bot.command('jetton_stop', async (ctx) => {
        const user = userTag(ctx);
        log('INFO', `/jetton_stop command`, { user });
        if (!spam.running) {
            log('WARN', `jetton_stop but spam not running`, { user });
            await ctx.reply('Spam is not running');
            return;
        }
        spam.abort = true;
        log('INFO', `jetton spam stopping`, { user, sent: spam.sent, failed: spam.failed });
        await ctx.reply(`Stopping jetton spam...\nSent: ${spam.sent}, Failed: ${spam.failed}`);
    });

    bot.command('external', async (ctx) => {
        const user = userTag(ctx);
        const text = getMessageText(ctx);
        log('INFO', `/external command`, { user, text });

        if (externalSpam.running) {
            log('WARN', `external spam already running`, {
                user,
                sent: externalSpam.sent,
                failed: externalSpam.failed,
            });
            await ctx.reply(`Already running. sent=${externalSpam.sent}, failed=${externalSpam.failed}`);
            return;
        }

        const parts = text.trim().split(/\s+/);
        const freqArg = parts[1];
        const huge = parts[2] === 'huge';

        if (!freqArg) {
            await ctx.reply('Usage: /external <freq>');
            return;
        }

        const freq = parsePositiveInt(freqArg);
        if (freq === null) {
            log('WARN', `/external invalid freq arg`, { user, freqArg });
            await ctx.reply('freq must be a positive integer. Usage: /external <freq>');
            return;
        }

        const periodMs = Math.floor(1000 / freq);
        externalSpam.running = true;
        externalSpam.abort = false;
        externalSpam.sent = 0;
        externalSpam.failed = 0;

        log('INFO', `external spam starting`, { user, freq, periodMs, wallets: SPAM_WALLET_COUNT, huge });
        await ctx.reply(
            `Starting external spam\nWallets: ${SPAM_WALLET_COUNT}\nFreq: ${freq} req/s (${periodMs}ms)${huge ? '\nHuge mode: ~40 cells / ~20000 bits per external' : ''}`,
        );

        runExternalSpamLoop(() => runtime, externalSpam, periodMs, freq, ctx, huge).catch((e) => {
            const details = extractErrorDetails(e);
            log('ERROR', `external loop crashed`, details);
            ctx.reply(`external loop crashed: ${details.error}`).catch(() => {});
            externalSpam.running = false;
        });
    });

    bot.command('external_stop', async (ctx) => {
        const user = userTag(ctx);
        log('INFO', `/external_stop command`, { user });
        if (!externalSpam.running) {
            log('WARN', `external_stop but external spam not running`, { user });
            await ctx.reply('External spam is not running');
            return;
        }
        externalSpam.abort = true;
        log('INFO', `external spam stopping`, { user, sent: externalSpam.sent, failed: externalSpam.failed });
        await ctx.reply(
            `Stopping external spam...\nSent: ${externalSpam.sent}, Failed: ${externalSpam.failed}`,
        );
    });

    bot.catch((error, ctx) => {
        log('ERROR', `unhandled bot error`, { updateId: ctx.update.update_id, ...extractErrorDetails(error) });
    });

    await bot.telegram.setMyCommands([
        { command: 'start', description: 'Start TPS on 1 shard — /start <tps>' },
        { command: 'stop', description: 'Stop TPS on 1 shard' },
        { command: 'shards', description: 'Start TPS on all 16 shards — /shards <tps per contract>' },
        { command: 'shards_stop', description: 'Stop TPS on all 16 shards' },
        { command: 'jetton', description: 'Start jetton spam — /jetton <freq>' },
        { command: 'jetton_stop', description: 'Stop jetton spam' },
        { command: 'external', description: 'Start external spam — /external <freq>' },
        { command: 'external_stop', description: 'Stop external spam' },
        { command: 'config', description: 'Upload liteserver config .json to use liteservers' },
        { command: 'api', description: 'Switch to HTTP backend — /api [url] [key]' },
    ]);

    await bot.launch();
    log('INFO', `bot polling started`);

    process.once('SIGINT', () => { log('INFO', `received SIGINT, shutting down`); bot.stop('SIGINT'); });
    process.once('SIGTERM', () => { log('INFO', `received SIGTERM, shutting down`); bot.stop('SIGTERM'); });
}

void main().catch((error) => {
    const message = error instanceof Error ? error.message : String(error);
    console.error(`Failed to launch telegramTpsBot: ${message}`);
    process.exit(1);
});
