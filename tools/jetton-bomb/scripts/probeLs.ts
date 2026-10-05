// Probe every liteserver in a global config: connect, fetch masterchain info,
// report alive/dead with latency. Probes run in parallel, each with its own
// short timeout so one stuck server doesn't block the rest.
//
// Usage:
//   npx ts-node scripts/probeLs.ts                       # default megaconfig-testnet.json
//   npx ts-node scripts/probeLs.ts ./megaconfig-testnet.json
//   LS_CONFIG=./testnet.json npx ts-node scripts/probeLs.ts
//   TIMEOUT_MS=3000 npx ts-node scripts/probeLs.ts

import * as fs from 'fs';
import * as path from 'path';
import { LiteClient, LiteSingleEngine } from 'ton-lite-client';

interface LiteServer {
    ip: number;
    port: number;
    provided?: string;
    id: { '@type': string; key: string };
}

interface GlobalConfig {
    liteservers: LiteServer[];
}

function intToIp(ip: number): string {
    if (ip < 0) ip += 0x100000000;
    return [(ip >>> 24) & 0xff, (ip >>> 16) & 0xff, (ip >>> 8) & 0xff, ip & 0xff].join('.');
}

// waitReady resolves when the engine fires 'ready' (ADNL handshake done),
// or rejects on engine 'error' / timeout. LiteSingleEngine starts with
// _closed=true and only flips it after the TCP 'connect' callback runs, so
// queries issued before then throw "Engine is closed" synchronously.
function waitReady(engine: LiteSingleEngine, ms: number): Promise<void> {
    return new Promise((resolve, reject) => {
        if (engine.isReady()) return resolve();
        const onReady = () => { cleanup(); resolve(); };
        const onError = () => { cleanup(); reject(new Error('socket error')); };
        const t = setTimeout(() => { cleanup(); reject(new Error(`connect timeout ${ms}ms`)); }, ms);
        const cleanup = () => {
            clearTimeout(t);
            engine.off('ready', onReady);
            engine.off('error', onError);
        };
        engine.once('ready', onReady);
        engine.once('error', onError);
    });
}

async function probe(ls: LiteServer, timeoutMs: number): Promise<{ ok: boolean; ms: number; info?: string; err?: string }> {
    const host = `tcp://${intToIp(ls.ip)}:${ls.port}`;
    const engine = new LiteSingleEngine({
        host,
        publicKey: Buffer.from(ls.id.key, 'base64'),
    });
    const client = new LiteClient({ engine });
    const started = Date.now();
    const deadline = started + timeoutMs;
    try {
        await waitReady(engine, Math.max(1, deadline - Date.now()));
        const remaining = Math.max(1, deadline - Date.now());
        const info = await Promise.race([
            client.getMasterchainInfo(),
            new Promise((_, rej) => setTimeout(() => rej(new Error(`query timeout ${remaining}ms`)), remaining)),
        ]);
        const ms = Date.now() - started;
        // @ts-expect-error: shape from lite-client, seqno lives on .last.seqno
        const seqno = info?.last?.seqno;
        return { ok: true, ms, info: `seqno=${seqno}` };
    } catch (e: any) {
        return { ok: false, ms: Date.now() - started, err: e?.message ?? String(e) };
    } finally {
        try {
            engine.close();
        } catch {}
    }
}

async function main() {
    const configPath =
        process.argv[2] ??
        process.env.LS_CONFIG ??
        path.join(__dirname, '..', 'megaconfig-testnet.json');
    const timeoutMs = Number(process.env.TIMEOUT_MS ?? 5000);

    if (!fs.existsSync(configPath)) {
        console.error(`config not found: ${configPath}`);
        process.exit(1);
    }
    const config = JSON.parse(fs.readFileSync(configPath, 'utf8')) as GlobalConfig;
    const servers = config.liteservers ?? [];
    if (servers.length === 0) {
        console.error('no liteservers in config');
        process.exit(1);
    }
    console.log(`Probing ${servers.length} liteservers from ${configPath} (timeout ${timeoutMs}ms each)\n`);

    const results = await Promise.all(servers.map((ls) => probe(ls, timeoutMs)));

    let okCount = 0;
    results.forEach((r, i) => {
        const ls = servers[i];
        const label = `${intToIp(ls.ip)}:${ls.port}${ls.provided ? ` (${ls.provided})` : ''}`;
        if (r.ok) {
            okCount++;
            console.log(`[OK   ${String(r.ms).padStart(4)}ms] ${label}  ${r.info}`);
        } else {
            console.log(`[FAIL ${String(r.ms).padStart(4)}ms] ${label}  ${r.err}`);
        }
    });

    console.log(`\n${okCount}/${servers.length} alive`);
    process.exit(okCount === servers.length ? 0 : 1);
}

main().catch((e) => {
    console.error(e);
    process.exit(1);
});
