import { sleep, NetworkProvider, UIProvider } from '@ton/blueprint';
import { Address, Cell, Transaction } from '@ton/core';
import { formatCoinsPure } from '@ton/sandbox/dist/utils/printTransactionFees';

export const promptBool = async (prompt: string, options: [string, string], ui: UIProvider) => {
    let yes = false;
    let no = false;
    let opts = options.map((o) => o.toLowerCase());

    do {
        let res = (await ui.input(prompt)).toLowerCase();
        yes = res == opts[0];
        if (!yes) no = res == opts[1];
    } while (!(yes || no));

    return yes;
};

export const promptAddress = async (prompt: string, provider: UIProvider, fallback?: Address) => {
    let promptFinal = fallback ? prompt.replace(/:$/, '') + `(default:${fallback}):` : prompt;
    do {
        let testAddr = (await provider.input(promptFinal)).replace(/^\s+|\s+$/g, '');
        try {
            return testAddr == '' && fallback ? fallback : Address.parse(testAddr);
        } catch (e) {
            provider.write(testAddr + ' is not valid!\n');
            prompt = 'Please try again:';
        }
    } while (true);
};

export const promptAmount = async (prompt: string, provider: UIProvider) => {
    let resAmount: number;
    do {
        let inputAmount = await provider.input(prompt);
        resAmount = Number(inputAmount);
        if (isNaN(resAmount)) {
            provider.write('Failed to convert ' + inputAmount + ' to float number');
        } else {
            return resAmount.toFixed(9);
        }
    } while (true);
};

export const waitForTransaction = async (
    provider: NetworkProvider,
    address: Address,
    curTx: string | bigint | null,
    maxRetry: number,
    interval: number = 1000,
) => {
    let done = false;
    let count = 0;
    const ui = provider.ui();

    const api = provider.api() as { getContractState?: (addr: Address) => Promise<any> };
    if (!api.getContractState) {
        ui.write('API does not support getContractState. Cannot wait for transaction.');
        return false;
    }

    do {
        ui.write(`Awaiting transaction completion (${++count}/${maxRetry})`);
        await sleep(interval);
        const curState = await api.getContractState(address);
        if (curState.lastTransaction !== null) {
            done = curState.lastTransaction.lt !== curTx;
        }
    } while (!done && count < maxRetry);
    return done;
};

export const displayContentCell = (content: Cell, ui: UIProvider) => {
    const cs = content.beginParse();
    const contentType = cs.loadUint(8);
    switch (contentType) {
        case 1:
            const noData = cs.remainingBits == 0;
            if (noData && cs.remainingRefs == 0) {
                ui.write('No data in content cell!\n');
            } else {
                const contentUrl = noData ? cs.loadStringRefTail() : cs.loadStringTail();
                ui.write(`Content metadata url:${contentUrl}\n`);
            }
            break;
        case 0:
            ui.write('On chain format not supported yet!\n');
            break;
        default:
            ui.write(`Unknown content format indicator:${contentType}\n`);
    }
};

export const promptUrl = async (prompt: string, ui: UIProvider) => {
    let retry = false;
    let input = '';
    let res = '';

    do {
        input = await ui.input(prompt);
        try {
            let testUrl = new URL(input);
            res = testUrl.toString();
            retry = false;
        } catch (e) {
            ui.write(input + " doesn't look like a valid url:\n" + e);
            retry = !(await promptBool('Use anyway?(y/n)', ['y', 'n'], ui));
        }
    } while (retry);
    return input;
};

function formatCoins(value: bigint | undefined, precision = 6): string {
    if (value === undefined) return 'N/A';

    return formatCoinsPure(value, precision) + ' TON';
}

export async function printReprChain(transactions: Transaction[]) {
    console.table(
        transactions
            .map((tx) => {
                if (tx.description.type !== 'generic') return undefined;

                if (tx.inMessage?.info.type !== 'internal') return undefined;

                const inBody = tx.inMessage?.body.beginParse();

                const op = inBody.loadUint(32);
                if (op !== 0xba5e5d50) return undefined;

                const last_subwallet = inBody.loadUint(32);
                let fromId = inBody.loadUint(32);

                let inTxType = 'deploy';
                if (last_subwallet !== 0) {
                    inTxType = 'deploy_hop';
                }

                let initSlice = tx.inMessage.init?.data?.beginParse();
                initSlice?.skip(32); // seqno
                const toId = initSlice?.loadUint(32) || 'no init';

                const deployed =
                    (tx.oldStatus == 'uninitialized' || tx.oldStatus == 'non-existing') && tx.endStatus == 'active';

                const outMsgs = tx.outMessages.size;

                // classic below:

                const valueInInt = tx.inMessage?.info.type === 'internal' ? tx.inMessage.info.value.coins : undefined;
                const valueIn = formatCoins(valueInInt);

                const valueOutInt = tx.outMessages
                    .values()
                    .reduce(
                        (total, message) => total + (message.info.type === 'internal' ? message.info.value.coins : 0n),
                        0n,
                    );

                const valueOut = formatCoins(valueOutInt);

                const computeFees = formatCoins(
                    tx.description.computePhase.type === 'vm' ? tx.description.computePhase.gasFees : undefined,
                );

                const balanceChange = formatCoins((valueInInt || 0n) - valueOutInt - tx.totalFees.coins);

                const exitCode =
                    tx.description.computePhase.type === 'vm' ? tx.description.computePhase.exitCode : 'N/A';

                let status = 'ok';
                if (exitCode !== 0) status = 'failed ' + exitCode.toString();

                return {
                    status,
                    fromId,
                    toId,
                    deployed,
                    outMsgs,
                    valueIn,
                    valueOut,
                    balanceChange,
                    outActions: tx.description.actionPhase?.totalActions ?? 'N/A',
                    computeFees,
                    exitCode,
                    actionCode: tx.description.actionPhase?.resultCode ?? 'N/A',
                };
            })
            .filter((v) => v !== undefined),
    );
}
