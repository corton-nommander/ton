import { Address, beginCell, Cell, SendMode, toNano } from '@ton/core';
import { compile, NetworkProvider, sleep } from '@ton/blueprint';
import { randomInt } from 'crypto';
import { JettonWallet } from '../wrappers/JettonWallet';
import { JettonMinter } from '../wrappers/JettonMinter';
import { WalletSpam, spamMsg } from '../wrappers/WalletSpam';
import { MINTERS } from './lib/addresses';
import { loadKeypairFromFile, formatPublicKey } from './lib/mnemonic';

// shard reference address для определения целевого шарда
// шард определяется как первые SHARD_BITS бит из hashpart этого адреса
const SHARD_REFERENCE_ADDRESS = Address.parse('0QD6a-uyjiAX8gRmtPi2ztdeBEliw6GrBa6dQBeM8wlQfJ5K');
// количество бит для шарда (2 = 4 шарда 0-3, 3 = 8 шардов 0-7 и т.д.)
const SHARD_BITS = 2;

// максимальное количество subwallets (минимум 100к задеплоено)
const MAX_SUBWALLET_ID = 100000;

// интервал между запусками новых отправок в миллисекундах
const SEND_INTERVAL_MS = 10;

/**
 * Получает шард адреса из первых SHARD_BITS бит hashpart
 */
function getShard(addr: Address): number {
    const firstByte = addr.hash.readUInt8();
    const shift = 8 - SHARD_BITS;
    return firstByte >> shift;
}

/**
 * Генерирует случайный адрес в заданном шарде
 */
function randomAddressInShard(targetShard: number, workchain: number = 0): Address {
    const buf = Buffer.alloc(32);
    for (let i = 0; i < buf.length; i++) {
        buf[i] = Math.floor(Math.random() * 256);
    }
    
    // устанавливаем первые SHARD_BITS бит в целевой шард
    const shift = 8 - SHARD_BITS;
    const mask = 0xff >> SHARD_BITS; // маска для оставшихся бит (0x3f для 2 бит, 0x0f для 4 бит)
    buf[0] = (buf[0] & mask) | (targetShard << shift);
    
    return new Address(workchain, buf);
}

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();

    const keypair = await loadKeypairFromFile(ui);
    if (!keypair) return;
    const keypairSafe = keypair;
    ui.write(`Pubkey: ${formatPublicKey(keypairSafe)}`);

    // определяем целевой шард из reference address
    const targetShard = getShard(SHARD_REFERENCE_ADDRESS);
    ui.write(`Target shard: ${targetShard} (first ${SHARD_BITS} bits)`);
    ui.write(`Send interval: ${SEND_INTERVAL_MS}ms`);

    const code = await compile('WalletSpam');
    const jminter = provider.open(JettonMinter.createFromAddress(MINTERS.pub4));
    const jwalletCode = (await jminter.getJettonData()).walletCode;

    let totalAttempts = 0;
    let successCount = 0;
    let failCount = 0;
    let lastStatsTime = Date.now();

    // функция для отправки одной транзакции (работает параллельно)
    async function sendOne() {
        const maxAttempts = 1000;
        let attempts = 0;

        while (attempts < maxAttempts) {
            attempts++;
            totalAttempts++;
            
            // выбираем случайный subwallet_id
            const subwalletId = randomInt(0, MAX_SUBWALLET_ID);
            
            const wallet = provider.open(
                WalletSpam.createFromConfig({ subwallet_id: subwalletId, keypair: keypairSafe, spam_msg: spamMsg }, code),
            );
            
            const walletShard = getShard(wallet.address);
            
            // проверяем шард subwallet
            if (walletShard !== targetShard) {
                continue;
            }

            // вычисляем адрес jetton wallet
            const jwallet = JettonWallet.createFromConfig(
                { owner: wallet.address, master: MINTERS.pub4, code: jwalletCode, optimized: true },
                jwalletCode,
            );
            
            const jettonWalletShard = getShard(jwallet.address);
            
            // проверяем шард jetton wallet
            if (jettonWalletShard !== targetShard) {
                continue;
            }

            // генерируем случайный адрес получателя в том же шарде (проверяем до совпадения)
            let randomDest: Address;
            let destShard: number;
            do {
                randomDest = randomAddressInShard(targetShard);
                destShard = getShard(randomDest);
            } while (destShard !== targetShard);

            // создаем transfer message БЕЗ комментария (пустой forward payload)
            const transferMsg = JettonWallet.transferMessage(
                1n, // 1 PUB4
                randomDest,
                wallet.address,
                Cell.EMPTY, // custom payload
                toNano('0.05'), // forward ton amount
                beginCell().endCell(), // пустой forward payload (без комментария)
            );

            try {
                await wallet.send({
                    to: jwallet.address,
                    value: toNano('0.16'),
                    bounce: true,
                    sendMode: SendMode.PAY_GAS_SEPARATELY,
                    body: transferMsg,
                    init: jwallet.init,
                });
                
                successCount++;
                ui.write(`[${successCount}] Sent 1 PUB4 from subwallet ${subwalletId} to ${randomDest.toString()}`);
                return; // успешно отправили, завершаем функцию
            } catch (e) {
                failCount++;
                // продолжаем искать другой subwallet
            }
        }
    }

    // запускаем непрерывный цикл отправок
    ui.write('Starting continuous stress test...');
    while (true) {
        // запускаем новую отправку без await (параллельно)
        sendOne().catch((e) => {
            failCount++;
            // игнорируем ошибки, продолжаем работу
        });
        
        // ждем интервал перед запуском следующей отправки
        await sleep(SEND_INTERVAL_MS);
        
        // периодически выводим статистику (раз в 10 секунд)
        const now = Date.now();
        if (now - lastStatsTime > 10000) {
            ui.write(`Stats: Success=${successCount}, Failed=${failCount}, Total attempts=${totalAttempts}`);
            lastStatsTime = now;
        }
    }
}
