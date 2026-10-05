import { Config } from '@ton/blueprint';

export const config: Config = {
    network: {
        endpoint: 'https://testnet.toncenter.com/api/v2/jsonRPC',
        // endpoint: 'http://devnet-01.toncenter.com/jsonRPC',
        type: 'testnet',
        version: 'v2',
        key: 'f101330b5396f62104d3151619be5a966f0521503704173aee407517114efc21',
        // key: '',
    },
};
