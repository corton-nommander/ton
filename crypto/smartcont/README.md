# Smart Contract Fift Scripts

## Native Transfer Wallets

Native transfer wallets are used with the protocol-level native transfer fast path. They do not deploy or call a TVM wallet contract. A signed native transfer external message is consumed by the validator and produces:

- `trans_native_transfer_debit`
- `trans_native_transfer_credit`

The native wallet account id is the Ed25519 public key bytes. Fund the generated address as a balance-only account before sending native transfers.

## Setup

```bash
cd /home/neodix/gitProjects/corton-nommander-ton-sidechain

export FIFTPATH="$PWD/crypto/fift/lib:$PWD/crypto/smartcont"

cmake --build build --target fift -j2
```

## Create Native Wallets

```bash
mkdir -p native-wallets

./build/crypto/fift -s crypto/smartcont/new-native-wallet.fif \
  0 native-wallets/alice

./build/crypto/fift -s crypto/smartcont/new-native-wallet.fif \
  0 native-wallets/bob
```

This creates:

```text
native-wallets/alice.pk
native-wallets/alice.addr
native-wallets/alice.pub
native-wallets/bob.pk
native-wallets/bob.addr
native-wallets/bob.pub
```

## Create Native Transfer Query

```bash
./build/crypto/fift -s crypto/smartcont/native-wallet.fif \
  native-wallets/alice \
  @native-wallets/bob.addr \
  0 \
  1 \
  -f 0.01 \
  native-wallets/alice-to-bob
```

This writes:

```text
native-wallets/alice-to-bob.boc
```

The command arguments are:

```text
native-wallet.fif <filename-base> <dest-addr> <nonce> <amount> [-f <fee>] [-t <timeout>|-u <unix-time>] [<savefile>]
```

For a freshly funded balance-only native account, use nonce `0`. After each successful native transfer, use the source account's printed `native_nonce` as the next nonce.
