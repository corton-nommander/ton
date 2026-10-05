import { NetworkProvider, sleep } from '@ton/blueprint';
import { Address, fromNano } from '@ton/core';
import { JettonWallet } from '../wrappers/JettonWallet';
import { randomInt } from 'crypto';
import { JettonMinter } from '../wrappers/JettonMinter';
import { MINTERS } from './lib/addresses';

const isOptimized = false;
const minter = isOptimized ? MINTERS.pub4 : MINTERS.classic;

export const destinations = [
    Address.parse('EQBrN0rSciMvlCp_hHQTehR8v8ybnZtDFRG7-geAD_jAAuz1'),
    Address.parse('EQCAFe8c_DhJr-_fBHqFjFnHdN8nitjZK7Hw61nV7Ptc_vsp'),
    Address.parse('EQB3jBvvzjCJ33NSIgkTp1sJUB0QV3_Do0rYZ37b9VTsBpwy'),
    Address.parse('EQC87kRCzLUD9ltDZVDDKEkfbSHlp8r-vs9PNjlGmSVhR-WX'),
    Address.parse('EQDwFLwfmVZPnuL_aUW169E97H4RQRz_rzHRMplNpn1GY6ud'),
    Address.parse('EQDHj4e4k6sG7bKlp2ZhfoI74snIW6lStms5wOgCexYfTp_M'),
    Address.parse('EQDcdPe9sBDG6aDY3LIVUpEnHY0Jzka6K4FXivVPEKtq9GSn'),
    Address.parse('EQCWZMoCljd2ZcXy4e2FnI4EKpHL69PSidNejgy7AqgM0Tir'),
    Address.parse('EQA0f3QUuG6gNYlcCskjR9xvXBjPhsS3Kgiegfefkfgrfwpz'),
    Address.parse('EQACEkHXL_xJ37mi2Gxhe4nsK3EsCqnIDfBHzR57D7pF29u_'),
    Address.parse('EQCo2qJWTH7w1OKNz38emFIzt4NIh9JwT2QGWnUpEQU7MvDV'),
    Address.parse('EQBRZRIhpx-D5_U_okkPVC3eAM6O-p-jAqnwI3p1Y4zCILie'),
    Address.parse('EQBHSqytyGCMQfojlIJy4UsvAUo2gytnWgwOPcXQgdHma3Bt'),
    Address.parse('EQAdCsECUNlMsdPULciQs9szK-zOSl2sP9CM4ijMG-SMpG24'),
    Address.parse('EQDqDdD1JTsIQIyQdFK1ITvytSORhHHXrFys7bwtARNfl6Jp'),
    Address.parse('EQArCssejMs5fh4ApwsvvKg-z4QcanaeOfivWwwnLUUrj6EW'),
];
export const getRandDest = () => destinations[randomInt(0, destinations.length)];

export async function run(provider: NetworkProvider) {
    const ui = provider.ui();
    let sumJtn = 0n;
    let sumEc = 0n;

    const jminter = provider.open(JettonMinter.createFromAddress(minter));
    const jwalletCode = (await jminter.getJettonData()).walletCode;

    async function getBalances(addr: Address) {
        const cont = provider.api().provider(addr);
        const state = await cont.getState();
        let ecbalance = 0n;
        if (state.extracurrency) ecbalance = state.extracurrency[100];
        sumEc += ecbalance;
        const jw = provider.open(
            JettonWallet.createFromConfig(
                { owner: addr, master: minter, code: jwalletCode, optimized: isOptimized },
                jwalletCode,
            ),
        );
        const jbalance = await jw.getJettonBalance();
        sumJtn += jbalance;
        console.log(`${addr.toString()}: PUB: ${jbalance}, EC: ${fromNano(ecbalance)}`);
    }

    for (let dest of destinations) {
        await getBalances(dest);
        await sleep(10);
    }
    await sleep(5000);
    console.log(`\nTotal: JTN: ${sumJtn}, EC: ${fromNano(sumEc)}`);
}
