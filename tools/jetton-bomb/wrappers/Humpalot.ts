import { Address, beginCell, Cell, Contract, contractAddress, ContractProvider, Sender, SendMode } from '@ton/core';

export const OP_GRIND = 0x48554d50; // "HUMP" — grind to a gas budget
export const OP_GRIND_LOOP = 0x48554d51; // grind to budget, self-continue
export const OP_GRIND_RND = 0x48554d52; // grind a fixed number of rounds

export type HumpalotConfig = {
    seqno?: number;
    roundsTotal?: bigint;
    bestHash?: bigint;
};

const MAX_HASH = (1n << 256n) - 1n;

export function humpalotConfigToCell(config: HumpalotConfig): Cell {
    return beginCell()
        .storeUint(config.seqno ?? 0, 32)
        .storeUint(config.roundsTotal ?? 0n, 64)
        .storeUint(config.bestHash ?? MAX_HASH, 256)
        .endCell();
}

export class Humpalot implements Contract {
    constructor(
        readonly address: Address,
        readonly init?: { code: Cell; data: Cell },
    ) {}

    static createFromAddress(address: Address) {
        return new Humpalot(address);
    }

    static createFromConfig(config: HumpalotConfig, code: Cell, workchain = 0) {
        const data = humpalotConfigToCell(config);
        const init = { code, data };
        return new Humpalot(contractAddress(workchain, init), init);
    }

    async sendDeploy(provider: ContractProvider, via: Sender, value: bigint) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell().endCell(),
        });
    }

    /**
     * Grind until the transaction has consumed `budget` gas, then stop. Adapts
     * to the network's per-tx gas limit — set budget just below it. `loop`
     * re-posts one continuation to self to self-sustain a chain.
     */
    async sendGrind(provider: ContractProvider, via: Sender, value: bigint, budget: number, loop = false) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell()
                .storeUint(loop ? OP_GRIND_LOOP : OP_GRIND, 32)
                .storeUint(budget, 32)
                .endCell(),
        });
    }

    /** Grind a fixed `rounds` HASHEXT_SHA256 iterations (deterministic). */
    async sendGrindRounds(provider: ContractProvider, via: Sender, value: bigint, rounds: number) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell().storeUint(OP_GRIND_RND, 32).storeUint(rounds, 32).endCell(),
        });
    }

    async getSeqno(provider: ContractProvider): Promise<number> {
        const { stack } = await provider.get('get_seqno', []);
        return stack.readNumber();
    }

    async getRoundsTotal(provider: ContractProvider): Promise<bigint> {
        const { stack } = await provider.get('get_rounds_total', []);
        return stack.readBigNumber();
    }

    async getBestHash(provider: ContractProvider): Promise<bigint> {
        const { stack } = await provider.get('get_best_hash', []);
        return stack.readBigNumber();
    }
}
