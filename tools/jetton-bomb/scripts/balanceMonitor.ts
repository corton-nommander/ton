import { Cell, fromNano } from '@ton/core';
import { NetworkProvider, sleep } from '@ton/blueprint';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';
import { WalletSpam, spamMsg } from '../wrappers/WalletSpam';
import { config as blueprintConfig } from '../blueprint.config';

// Usage: COUNT=1000 yarn blueprint run balanceMonitor
// Optional: BATCH=256 (default 256, max 1000)

type AccountState = {
    address: string;
    balance: string;
    status: string;
};

function getV3Endpoint(): string {
    const cfg = typeof blueprintConfig.network === 'object' ? blueprintConfig.network : null;
    const v2 = process.env.TONCENTER_ENDPOINT?.trim() || cfg?.endpoint || '';
    // derive v3 from v2: replace /api/v2/... with /api/v3
    return v2.replace(/\/api\/v2.*/, '/api/v3');
}

function getApiKey(): string | undefined {
    const cfg = typeof blueprintConfig.network === 'object' ? blueprintConfig.network : null;
    return process.env.TONCENTER_API_KEY?.trim() || cfg?.key || undefined;
}

async function fetchBalances(addresses: string[], endpoint: string, apiKey?: string): Promise<AccountState[]> {
    const params = new URLSearchParams();
    for (const addr of addresses) {
        params.append('address', addr);
    }
    params.set('include_boc', 'false');

    const url = `${endpoint}/accountStates?${params.toString()}`;
    const headers: Record<string, string> = {};
    if (apiKey) headers['X-API-Key'] = apiKey;

    const res = await fetch(url, { headers });
    if (!res.ok) throw new Error(`API error: ${res.status} ${await res.text()}`);
    const data = await res.json();
    return data.accounts as AccountState[];
}

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const count = Number(process.env.COUNT);
    if (!count) {
        ui.write('COUNT is required');
        return;
    }
    const batchSize = Math.min(Number(process.env.BATCH || 256), 1000);

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    ui.write(`Pubkey: ${formatPublicKey(keypair)}`);

    const oldcode =
        'b5ee9c7241020c010001bb000114ff00f4a413f4bcf2c80b01020120020b020148030601e2d03221c700915be0d0d3030171b0915be001d31f018210ba5e5d50bd915be0d31fd31f3002fa4030ed44d0d31f31d31fd3ffd4302070fb005324be925f06e054351570552003c8cb1f12cb1fcbffccc970f82a21c8cb01f40012f400cb00c9f9008309c8cb0acbffc9d05003c705f2e19104015281ff77f833206e913099d0d70b0fc001f2d29ae2708212540be40072fb025331a17f6d91218ae85f080500fc705354a0a4537903c8cb1f12cb1fcbffccc970f82a21c8cb01f40012f400cb00c920f9008309c8cb0acbffc9d0708212540be400712509a4208100c8be5319beb19d375f033570287083066d090407de8210ba5e5d50c8cb1f14cb1f52a0cb1fc9c8801801cb055005cf1658fa0218f4005a775003cb6accccc95005fb00020120070a02012008090017bb39ced44d0d33f31d70bff80011b8c97ed44d0d70b1f80017bfdddf6a268698f98eb858fc00a2f28308d71820d31fd31fd31f02f823bbf263ed44d0d31fd31fd3ffd4305143baf2a15151baf2a205f901541064f910f2a3f800019320d74a96d307d402fb00e83003a40303c8cb1f12cb1fcbffccc9ed5496ba1903';
    const code = Cell.fromBoc(Buffer.from(oldcode, 'hex'))[0];

    // Pre-compute all addresses
    const addressMap = new Map<string, number>(); // raw -> id
    for (let i = 0; i < count; i++) {
        const wallet = WalletSpam.createFromConfig({ subwallet_id: i, keypair, spam_msg: spamMsg }, code);
        addressMap.set(wallet.address.toRawString(), i);
    }
    const allAddresses = Array.from(addressMap.keys());
    ui.write(`Computed ${allAddresses.length} addresses, batch size: ${batchSize}`);

    const endpoint = getV3Endpoint();
    const apiKey = getApiKey();
    ui.write(`API: ${endpoint}`);

    let checked = 0;
    let totalBalance = 0n;
    let empty = 0;
    let uninit = 0;
    let min = { id: -1, balance: BigInt('999999999999999999') };
    let max = { id: -1, balance: 0n };

    // Shuffle and batch
    const shuffled = [...allAddresses].sort(() => Math.random() - 0.5);

    for (let i = 0; i < shuffled.length; i += batchSize) {
        const batch = shuffled.slice(i, i + batchSize);
        try {
            const accounts = await fetchBalances(batch, endpoint, apiKey);

            for (const acc of accounts) {
                const id = addressMap.get(acc.address) ?? -1;
                const balance = BigInt(acc.balance);
                checked++;
                totalBalance += balance;

                if (balance === 0n) empty++;
                if (acc.status === 'uninit') uninit++;
                if (balance < min.balance) min = { id, balance };
                if (balance > max.balance) max = { id, balance };
            }

            const avg = checked > 0 ? totalBalance / BigInt(checked) : 0n;
            ui.write(
                `checked: ${checked}/${count} | avg: ${fromNano(avg)} | min: ${fromNano(min.balance)} (#${min.id}) | max: ${fromNano(max.balance)} (#${max.id}) | empty: ${empty} | uninit: ${uninit}`,
            );
        } catch (e) {
            ui.write(`batch error: ${e}`);
        }
        await sleep(200);
    }

    const avg = checked > 0 ? totalBalance / BigInt(checked) : 0n;
    ui.write(`\nDone. Total checked: ${checked}/${count}`);
    ui.write(`Avg: ${fromNano(avg)} | Min: ${fromNano(min.balance)} (#${min.id}) | Max: ${fromNano(max.balance)} (#${max.id})`);
    ui.write(`Empty: ${empty} | Uninit: ${uninit} | Total balance: ${fromNano(totalBalance)}`);
}
