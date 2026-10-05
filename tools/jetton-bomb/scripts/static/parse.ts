import { Address, Cell } from '@ton/core';
import { TonClient4 } from '@ton/ton';
import { randomAddress } from '@ton/test-utils';

async function main() {
    const endpointV4 = `https://sandbox-v4.tonhubapi.com`;

    console.log(Address.parse('kQBvuUpLp7-ykZpNLk7C8Oru0O8EH_-vpmtv0ansB6Jxgy8D'));
    let addrreses: Map<number, Address> = new Map();

    while (addrreses.size < 16) {
        const r = randomAddress(0);
        const shard = r.hash.readUint8() >> 4;
        if (!addrreses.has(shard)) {
            addrreses.set(shard, r);
        }
    }
    console.log(addrreses.values());

    // const clientV4 = new TonClient4({
    //     endpoint: endpointV4,
    //     timeout: 20000,
    //     requestInterceptor: (config) => {
    //         config.headers['Content-Type'] = 'application/json';
    //         return config;
    //     },
    // });
    // const tx = (
    //     await clientV4.getAccountTransactions(
    //         Address.parse('kQBvuUpLp7-ykZpNLk7C8Oru0O8EH_-vpmtv0ansB6Jxgy8D'),
    //         31950125000014n,
    //         Buffer.from('50eb37ddc9e56fce871dc6e5d8c4f1592184197c9272cbf644d9c23034e5424e', 'hex'),
    //     )
    // )[0];
    // console.log(tx);

    // let cs = Cell.fromBase64('te6ccgEBAQEAKgAAUAAAAvgpqaMXD6aiGgmHPli5nujZp3ZigiFAh1+neQR2jFlnOogEvEc=').beginParse();
    // cs.loadUint(32);
    // let subwallet = cs.loadUint(32);
    // console.log(subwallet);
}

main();
