import {
    Address,
    beginCell,
    Cell,
    Contract,
    ContractABI,
    contractAddress,
    ContractProvider,
    Sender,
    SendMode
} from '@ton/core';

export type PingPongConfig = {
    ownerAddress: Address;
    isRunning: boolean;
    counter: bigint;
    limit: bigint;
};

export function pingPongConfigToCell(config: PingPongConfig): Cell {
    return beginCell()
        .storeAddress(config.ownerAddress)
        .storeBit(config.isRunning)
        .storeUint(config.counter, 64)
        .storeUint(config.limit, 64)
        .endCell();
}

export class PingPong implements Contract {
    abi: ContractABI = { name: 'PingPong' }

    constructor(readonly address: Address, readonly init?: { code: Cell; data: Cell }) {}

    static createFromAddress(address: Address) {
        return new PingPong(address);
    }

    static createFromConfig(config: PingPongConfig, code: Cell, workchain = 0) {
        const data = pingPongConfigToCell(config);
        const init = { code, data };
        return new PingPong(contractAddress(workchain, init), init);
    }

    async sendDeploy(provider: ContractProvider, via: Sender, value: bigint) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell().endCell(),
        });
    }

    async sendRun(provider: ContractProvider, via: Sender, value: bigint, limit: bigint) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell()
                .storeUint(0x5ce7c1d2, 32) // RunMsg opcode
                .storeUint(limit, 64)       // limit
                .endCell(),
        });
    }

    async sendStop(provider: ContractProvider, via: Sender, value: bigint) {
        await provider.internal(via, {
            value,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell()
                .storeUint(0x07c32b3f, 32) // StopMsg opcode
                .endCell(),
        });
    }

    async getIsRunning(provider: ContractProvider): Promise<boolean> {
        const result = await provider.get('isRunning', []);
        return result.stack.readNumber() !== 0;
    }

    async getOwner(provider: ContractProvider): Promise<Address> {
        const result = await provider.get('owner', []);
        return result.stack.readAddress();
    }

    async getCounter(provider: ContractProvider): Promise<bigint> {
        const result = await provider.get('counter', []);
        return result.stack.readBigNumber();
    }

    async getLimit(provider: ContractProvider): Promise<bigint> {
        const result = await provider.get('limit', []);
        return result.stack.readBigNumber();
    }

    async getPingPongInfo(provider: ContractProvider): Promise<[boolean, Address, bigint, bigint]> {
        const result = await provider.get('getPingPongInfo', []);
        return [result.stack.readNumber() !== 0, result.stack.readAddress(), result.stack.readBigNumber(), result.stack.readBigNumber()];
    }
}
