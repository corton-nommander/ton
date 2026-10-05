import { Address, toNano } from '@ton/core';
import {
    JettonMinter,
    JettonMinterContent,
    jettonContentToCell,
    jettonMinterConfigToCell,
} from '../wrappers/JettonMinter';
import { compile, NetworkProvider, UIProvider } from '@ton/blueprint';
import { promptAddress, promptBool, promptUrl } from '../wrappers/ui-utils';

const formatUrl =
    'https://github.com/ton-blockchain/TEPs/blob/master/text/0064-token-data-standard.md#jetton-metadata-example-offchain';

// https://gist.githubusercontent.com/1IxI1/58be1ba89431da8a562254bf2a58ba79/raw/668bebc6cd3f9075671f93585941c902c956d805/gistfile1.txt
const PUB4_CONTENT =
    'https://gist.githubusercontent.com/1IxI1/a49277da081f6e27811b6d5593730286/raw/12b434408399a1500f2f81a668f95a484b4b2f1b/pub4.json';

const exampleContent = {
    name: 'Sample Jetton',
    description: 'Sample of Jetton',
    symbol: 'JTN',
    decimals: 0,
    image: 'https://www.svgrepo.com/download/483336/coin-vector.svg',
};

const urlPrompt = 'Please specify url pointing to jetton metadata(json):';

export async function run(provider: NetworkProvider) {
    // const ui = provider.ui();
    // const sender = provider.sender();
    // const adminPrompt = `Please specify admin address`;
    // ui.write(`Jetton deployer\nCurrent deployer onli supports off-chain format:${formatUrl}`);

    // let admin = await promptAddress(adminPrompt, ui, sender.address);
    // ui.write(`Admin address:${admin}\n`);
    // let contentUrl = await promptUrl(urlPrompt, ui);
    // ui.write(`Jetton content url:${contentUrl}`);

    // let dataCorrect = false;
    // do {
    //     ui.write('Please verify data:\n');
    //     ui.write(`Admin:${admin}\n\n`);
    //     ui.write('Metadata url:' + contentUrl);
    //     dataCorrect = await promptBool('Is everything ok?(y/n)', ['y', 'n'], ui);
    //     if (!dataCorrect) {
    //         const upd = await ui.choose('What do you want to update?', ['Admin', 'Url'], (c) => c);

    //         if (upd == 'Admin') {
    //             admin = await promptAddress(adminPrompt, ui, sender.address);
    //         } else {
    //             contentUrl = await promptUrl(urlPrompt, ui);
    //         }
    //     }
    // } while (!dataCorrect);

    // const content = jettonContentToCell({ type: 1, uri: contentUrl });
    const content = jettonContentToCell({ type: 1, uri: PUB4_CONTENT });
    const admin = Address.parse('0:646f6f1f13124496504bd65441bc0d0677645a3b1e460896f0a299f13e53625a');

    const wallet_code = await compile('JettonWallet');

    const minter = provider.open(JettonMinter.createFromConfig({ admin, content, wallet_code }, await compile('JettonMinter')));
    console.log(minter.address);

    await minter.sendDeploy(provider.sender(), toNano('0.05'));
    // await provider.deploy(minter, toNano('0.05'));
}
