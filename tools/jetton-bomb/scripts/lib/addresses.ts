import { Address } from '@ton/core';

export const MINTERS = {
    classic: Address.parse('kQASuHm-bfj2Va-LagX-sqviV5tDoDaOTJZ8OBp3phwEI3v6'), // PUB classic
    pub4: Address.parse('EQBECJWGkqPCLTugqIFQPK_s6NU-nfzkLG0mxdiu2hdCxwdw'), // PUB4
    pub4Alt: Address.parse('kQBLNBxAPJ_jXKqq_7x72IvQ1VmCl6YKf7S8Usy624Ot13xH'), // PUB4 (alt)
};

export const DEFAULT_MINTER = MINTERS.pub4;
