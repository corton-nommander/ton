import { Address, beginCell, Cell, Contract, contractAddress, ContractProvider, Sender, SendMode } from '@ton/core';

export const OP_GROW = 0x46415447; // "FATG"
export const OP_POKE = 0x46415450; // "FATP"
export const OP_RESET = 0x46415452; // "FATR"

export type FatbastardConfig = {
    nextKey?: bigint;
    cellsHint?: bigint;
    nonce?: bigint;
};

export function fatbastardConfigToCell(config: FatbastardConfig): Cell {
    return beginCell()
        .storeDict(null) // empty store dict
        .storeUint(config.nextKey ?? 0n, 64)
        .storeUint(config.cellsHint ?? 0n, 64)
        .storeUint(config.nonce ?? 0n, 64)
        .endCell();
}

export class Fatbastard implements Contract {
    constructor(
        readonly address: Address,
        readonly init?: { code: Cell; data: Cell },
    ) {}

    static createFromAddress(address: Address) {
        return new Fatbastard(address);
    }

    static createFromConfig(config: FatbastardConfig, code: Cell, workchain = 0) {
        const data = fatbastardConfigToCell(config);
        const init = { code, data };
        return new Fatbastard(contractAddress(workchain, init), init);
    }

    async sendDeploy(provider: ContractProvider, via: Sender, value: bigint) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell().endCell(),
        });
    }

    /**
     * Add up to `count` entries, each a chain of `depth` unique cells
     * (+depth+1 cells each). Stops early once `budget` gas is consumed, so an
     * oversized count adapts to the network's per-tx gas limit and commits the
     * partial growth instead of reverting.
     */
    async sendGrow(
        provider: ContractProvider,
        via: Sender,
        value: bigint,
        count: number,
        depth: number,
        budget?: number,
    ) {
        let b = beginCell().storeUint(OP_GROW, 32).storeUint(count, 32).storeUint(depth, 16);
        if (budget !== undefined) {
            b = b.storeUint(budget, 32);
        }
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: b.endCell(),
        });
    }

    /** Cheap touch that mutates the nonce, forcing a full storage-dict re-walk. */
    async sendPoke(provider: ContractProvider, via: Sender, value: bigint) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell().storeUint(OP_POKE, 32).endCell(),
        });
    }

    async sendReset(provider: ContractProvider, via: Sender, value: bigint) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell().storeUint(OP_RESET, 32).endCell(),
        });
    }

    async getNextKey(provider: ContractProvider): Promise<bigint> {
        const { stack } = await provider.get('get_next_key', []);
        return stack.readBigNumber();
    }

    async getCellsHint(provider: ContractProvider): Promise<bigint> {
        const { stack } = await provider.get('get_cells_hint', []);
        return stack.readBigNumber();
    }

    async getNonce(provider: ContractProvider): Promise<bigint> {
        const { stack } = await provider.get('get_nonce', []);
        return stack.readBigNumber();
    }
}
