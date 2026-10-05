import { sleep, UIProvider } from '@ton/blueprint';

export async function monitorDeployment(
    ui: UIProvider,
    target: number,
    getSeqno: (subwalletId: number) => Promise<number>,
) {
    async function findLastDeployed() {
        let left = 0;
        let right = target;
        while (left < right) {
            const middle = Math.floor((left + right) / 2);
            const seqno = await getSeqno(middle);
            if (seqno === -1) {
                right = middle - 1;
            } else {
                left = middle + 1;
            }
        }
        return left;
    }

    let lastDeployed = await findLastDeployed();
    ui.setActionPrompt(`Deployed: ${lastDeployed}/${target}`);

    while (true) {
        const seqno = await getSeqno(lastDeployed + 2);
        if (seqno !== -1) {
            lastDeployed = await findLastDeployed();
            ui.setActionPrompt(`Deployed: ${lastDeployed}/${target}`);
        }
        if (lastDeployed === target) {
            break;
        }
        await sleep(2000);
    }

    ui.clearActionPrompt();
    ui.write(`Deployed all ${target}`);
}
