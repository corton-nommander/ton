import {
    Address,
    beginCell,
    Cell,
    Contract,
    contractAddress,
    ContractProvider,
    Sender,
    SendMode,
    toNano,
    storeStateInit,
    StateInit,
    internal,
    storeMessage,
    storeMessageRelaxed,
    MessageRelaxed,
    SenderArguments,
    Dictionary,
    DictionaryValue,
    loadMessageRelaxed,
} from '@ton/core';
import { KeyPair, sign } from '@ton/crypto';
import { Treasury } from '@ton/sandbox';

export type WalletRConfig = {
    subwallet_id: number;
    keypair: KeyPair;
};

export const MIN_BALANCE = toNano('1');

export function walletRConfigToCell(config: WalletRConfig): Cell {
    return beginCell()
        .storeUint(0, 32)
        .storeUint(config.subwallet_id, 32)
        .storeBuffer(config.keypair.publicKey, 32)
        .endCell();
}

function senderArgsToMessageRelaxed(args: SenderArguments): MessageRelaxed {
    return internal({
        to: args.to,
        value: args.value,
        init: args.init,
        body: args.body,
        bounce: args.bounce,
    });
}

export class WalletR implements Contract {
    constructor(
        readonly address: Address,
        readonly config: WalletRConfig,
        readonly init?: { code: Cell; data: Cell },
    ) {}

    static createFromAddress(address: Address, config: WalletRConfig) {
        return new WalletR(address, config);
    }

    static createFromConfig(config: WalletRConfig, code: Cell, workchain = 0) {
        const data = walletRConfigToCell(config);
        const init = { code, data };
        return new WalletR(contractAddress(workchain, init), config, init);
    }

    async sendDeploy(provider: ContractProvider, via: Sender, value: bigint, currency?: bigint) {
        await provider.internal(via, {
            value,
            extracurrency: currency
                ? {
                      100: currency,
                  }
                : undefined,
            sendMode: SendMode.PAY_GAS_SEPARATELY,
            body: beginCell().endCell(),
        });
    }

    async sendMessages(provider: ContractProvider, messages: MessageRelaxed[], sendMode?: SendMode) {
        let seqno = await this.getSeqno(provider);
        let transfer = this.createTransfer({
            sendMode: sendMode,
            messages: messages,
            seqno: seqno,
        });
        await provider.external(transfer);
    }

    async send(provider: ContractProvider, args: SenderArguments, seqno?: number) {
        seqno = seqno ?? (await this.getSeqno(provider));
        let transfer = this.createTransfer({
            sendMode: args.sendMode ?? undefined,
            messages: [senderArgsToMessageRelaxed(args)],
            seqno: seqno,
        });
        await this.signAndSendExternal(provider, transfer);
    }

    async signAndSendExternal(provider: ContractProvider, msg: Cell) {
        const hash = msg.hash();
        const signature = sign(hash, this.config.keypair.secretKey);
        const query = beginCell()
            .storeBuffer(signature, 64) // 512 bits signature
            .storeSlice(msg.asSlice()) // the rest - message
            .endCell();
        await provider.external(query);
    }

    async sendReproduce(provider: ContractProvider, last_subwallet: number, tonAmount?: bigint, until?: number) {
        const nextWallet = WalletR.createFromConfig(
            { subwallet_id: this.config.subwallet_id + 1, keypair: this.config.keypair },
            this.init!.code,
        );
        await this.send(provider, {
            to: nextWallet.address,
            value: tonAmount || toNano(last_subwallet * 1.1),
            init: nextWallet.init!,
            body: beginCell()
                .storeUint(0xba5e5d50, 32) // op::reproduce
                .storeUint(last_subwallet, 32)
                .storeUint(this.config.subwallet_id, 32)
                .endCell(),
        });
    }

    // deprecated
    async sendMsgAsWallet(provider: ContractProvider, fullMsg: Cell, sendMode: SendMode, until?: number) {
        let seqno = await this.getSeqno(provider);
        if (seqno == -1) seqno = 0;
        const msg = beginCell()
            .storeUint(this.config.subwallet_id, 32)
            .storeUint(until || Math.floor(Date.now() / 1000) + 60, 32) // valid_until
            .storeUint(seqno, 32)
            .storeUint(sendMode, 8)
            .storeRef(fullMsg)
            .endCell();
        await this.signAndSendExternal(provider, msg);
    }

    async getSeqno(provider: ContractProvider) {
        try {
            const { stack } = await provider.get('seqno', []);
            return stack.readNumber();
        } catch (e) {
            return -1;
        }
    }

    async getBalance(provider: ContractProvider): Promise<bigint> {
        return (await provider.getState()).balance;
    }

    getSender(provider: ContractProvider): Treasury {
        return {
            address: this.address,
            send: async (args) => {
                await this.send(provider, args);
            },
        };
    }

    createTransfer(args: { messages: MessageRelaxed[]; sendMode?: SendMode; until?: number; seqno: number }) {
        let msg = beginCell()
            .storeUint(this.config.subwallet_id, 32)
            .storeUint(args.until || Math.floor(Date.now() / 1000) + 40, 32) // valid_until
            .storeUint(args.seqno, 32);
        for (let messageCell of args.messages) {
            msg = msg.storeUint(args.sendMode || 0, 8).storeRef(beginCell().store(storeMessageRelaxed(messageCell)));
        }
        return msg.endCell();
    }
}
