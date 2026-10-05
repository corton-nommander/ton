import {
    Address,
    beginCell,
    Cell,
    Contract,
    contractAddress,
    ContractProvider,
    Sender,
    SendMode,
} from '@ton/core';

export type StaticTpsConfig = {
    ownerAddress: Address;
    nonce?: number; // packed into the targetTps slot to randomize address; sendRun overwrites it
};

export function staticTpsConfigToCell(config: StaticTpsConfig): Cell {
    return beginCell()
        .storeAddress(config.ownerAddress)
        .storeUint(0, 1) // isRunning
        .storeUint(config.nonce ?? 0, 32) // targetTps (nonce until first run)
        .storeUint(0, 32 + 64 + 64 + 32) // windowStart + windowTxCount + totalTxCount + activeEpoch
        .endCell();
}

export const Opcodes = {
    run: 0x5ce7c1d2,
    stop: 0x07c32b3f,
    work: 0x10adbeef,
    continue: 0xc0c0c0c0,
};

export class StaticTps implements Contract {
    constructor(
        readonly address: Address,
        readonly init?: { code: Cell; data: Cell },
    ) {}

    static createFromAddress(address: Address) {
        return new StaticTps(address);
    }

    static createFromConfig(config: StaticTpsConfig, code: Cell, workchain = 0) {
        const data = staticTpsConfigToCell(config);
        const init = { code, data };
        return new StaticTps(contractAddress(workchain, init), init);
    }

    async sendDeploy(provider: ContractProvider, via: Sender, value: bigint) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell().endCell(),
        });
    }

    async sendRun(provider: ContractProvider, via: Sender, value: bigint, targetTps: number) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell().storeUint(Opcodes.run, 32).storeUint(targetTps, 32).endCell(),
        });
    }

    async sendStop(provider: ContractProvider, via: Sender, value: bigint) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell().storeUint(Opcodes.stop, 32).endCell(),
        });
    }

    async getIsRunning(provider: ContractProvider): Promise<number> {
        const result = await provider.get('isRunning', []);
        return result.stack.readNumber();
    }

    async getOwner(provider: ContractProvider): Promise<Address> {
        const result = await provider.get('owner', []);
        return result.stack.readAddress();
    }

    async getTargetTps(provider: ContractProvider): Promise<number> {
        const result = await provider.get('targetTps', []);
        return result.stack.readNumber();
    }

    async getWindowTxCount(provider: ContractProvider): Promise<bigint> {
        const result = await provider.get('windowTxCount', []);
        return result.stack.readBigNumber();
    }

    async getTotalTxCount(provider: ContractProvider): Promise<bigint> {
        const result = await provider.get('totalTxCount', []);
        return result.stack.readBigNumber();
    }

    async getActiveEpoch(provider: ContractProvider): Promise<number> {
        const result = await provider.get('activeEpoch', []);
        return result.stack.readNumber();
    }

    async getMaxTps(provider: ContractProvider): Promise<number> {
        const result = await provider.get('maxTps', []);
        return result.stack.readNumber();
    }

    async getInfo(provider: ContractProvider) {
        const result = await provider.get('getInfo', []);
        return {
            isRunning: result.stack.readNumber(),
            owner: result.stack.readAddress(),
            targetTps: result.stack.readNumber(),
            windowStart: result.stack.readNumber(),
            windowTxCount: result.stack.readBigNumber(),
            totalTxCount: result.stack.readBigNumber(),
            activeEpoch: result.stack.readNumber(),
        };
    }
}
