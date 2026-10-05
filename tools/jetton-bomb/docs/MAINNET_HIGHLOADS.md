# Mainnet gasless relayers

Verified against `tc-apps-01`'s mainnet pool key on 2026-10-01.
The mainnet bot defaults to **four** Highload V3 wallets; the testnet bot can select the
original **16** with `HIGHLOAD_SHARDS=16`. Both use `HIGHLOAD_TTL=3600`.
The mainnet four each serve one top-two-bit address quarter (maximum four shards).
These retain the old set's nibble 0/4/8/c wallets; their keys, code, storage, and addresses are unchanged.

These are non-bounceable **UQ** forms for funding; EQ forms refer to the same accounts.
At the initial balance check these four were undeployed with zero balance. Funding alone does not
deploy their code: a separate `cmd/highload deploy` must supply `StateInit`. The bot does not
deploy or top up relayers. Deployment is currently deferred.

| Shard quarter | Address range | Mainnet funding address |
|---|---|---|
| 0 | `0:00…`–`0:3f…` | `UQANpaxjolN9ftbLhugcTyH15twKeFl76b19xFyLbCZBEXbR` |
| 1 | `0:40…`–`0:7f…` | `UQBPHJV3VlH8OSF35n9XCKBvIvLB98aI7TyD3ja7UosrtoUB` |
| 2 | `0:80…`–`0:bf…` | `UQCJcVPP-bws1PFY6pDpPCoaLD_w1yrDTpSC0X9kuzkhRzCo` |
| 3 | `0:c0…`–`0:ff…` | `UQDHP3d-6CBM4FuCiaP3oev_KbpWJwWvmAOhlX2q6GvS1keM` |

From the mainnet checkout, rederive or inspect the set with:

```bash
go -C go-spam run ./cmd/highload grind --pool-key ../jetton-prepaid/pool.key --ttl 3600
go -C go-spam run ./cmd/highload status --pool-key ../jetton-prepaid/pool.key --ttl 3600 \
  --endpoint https://toncenter.com/api/v2
```

The inspection command accepts `TONCENTER_API_KEY` from the environment. Funding amounts depend
on offered gasless transfers/s and duration; each native request receives 0.002 GRAM by default,
plus the highload's batch external/self-message/forwarding fees.
