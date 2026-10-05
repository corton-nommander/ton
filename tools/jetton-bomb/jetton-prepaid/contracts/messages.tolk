type ForwardPayloadRemainder = RemainingBitsAndRefs

struct (0b0) PayloadInline {
    value: RemainingBitsAndRefs
}

struct (0b1) PayloadInRef {
    value: Cell<RemainingBitsAndRefs>
}

struct (0x0f8a7ea5) AskToTransfer {
    queryId: uint64
    jettonAmount: coins
    transferRecipient: address
    sendExcessesTo: address?
    customPayload: cell?
    forwardTonAmount: coins
    @abi.clientType(PayloadInline | PayloadInRef)
    forwardPayload: ForwardPayloadRemainder
}

struct (0x7362d09c) TransferNotificationForRecipient {
    queryId: uint64
    jettonAmount: coins
    transferInitiator: address?
    @abi.clientType(PayloadInline | PayloadInRef)
    forwardPayload: ForwardPayloadRemainder
}

struct (0x178d4519) InternalTransferStep {
    queryId: uint64
    jettonAmount: coins
    transferInitiator: address?
    sendExcessesTo: address?
    forwardTonAmount: coins
    @abi.clientType(PayloadInline | PayloadInRef)
    forwardPayload: ForwardPayloadRemainder
}

struct (0xd53276db) ReturnExcessesBack {
    queryId: uint64
}

struct (0x595f07bc) AskToBurn {
    queryId: uint64
    jettonAmount: coins
    sendExcessesTo: address?
    customPayload: cell?
}

struct (0x7bdd97de) BurnNotificationForMinter {
    queryId: uint64
    jettonAmount: coins
    burnInitiator: address
    sendExcessesTo: address?
}

struct (0x2c76b973) RequestWalletAddress {
    queryId: uint64
    ownerAddress: address
    includeOwnerAddress: bool
}

struct (0xd1735400) ResponseWalletAddress {
    queryId: uint64
    jettonWalletAddress: address?
    ownerAddress: Cell<address>?
}

struct (0x642b7d07) MintNewJettons {
    queryId: uint64
    mintRecipient: address
    tonAmount: coins
    internalTransferMsg: Cell<InternalTransferStep>
}

struct (0x6501f354) ChangeMinterAdmin {
    queryId: uint64
    newAdminAddress: address
}

struct (0xfb88e119) ClaimMinterAdmin {
    queryId: uint64
}

struct (0x7431f221) DropMinterAdmin {
    queryId: uint64
}

struct (0x2508d66a) UpgradeMinterCode {
    queryId: uint64
    newData: cell
    newCode: cell
}

struct (0xcb862902) ChangeMinterMetadata {
    queryId: uint64
    newMetadata: cell
}

struct (0xd372158c) TopUpGrams {}

// Wallet V5-compatible signed-internal request primitives. `OutActionsCell` is the raw TVM c5
// OutList; WalletSpam accepts only action_send_msg nodes and does not support W5 extra actions.
type OutActionsCell = cell

struct (0x0ec3c86d) OutActionWithSendMessageOnly {
    prev: Cell<OutActionWithSendMessageOnlyOrEmpty>
    sendMode: uint8
    outMsg: cell
}

type OutActionWithSendMessageOnlyOrEmpty = OutActionWithSendMessageOnly | void

struct (0x73696e74) InternalSignedRequest {
    walletId: uint32
    validUntil: uint32
    seqno: uint32
    @abi.clientType(Cell<OutActionWithSendMessageOnly>?)
    outActions: OutActionsCell?
    hasExtraActions: bool
    extraActions: RemainingBitsAndRefs
    // The trailing 512-bit signature is parsed separately by WalletSpam.
}

// Custom load-tool op (ex-FunC op::withdraw_tons): owner sweeps the ENTIRE native
// GRAM balance out of this throwaway test wallet and self-destructs it.
// `to` = optional custom destination for the swept GRAM (null => owner).
struct (0x6d8e5e3c) WithdrawGrams {
    queryId: uint64
    to: address?
}

fun ForwardPayloadRemainder.checkIsCorrectTlbEither(self) {
    var mutableCopy = self;
    if (mutableCopy.loadMaybeRef() != null) {
        mutableCopy.assertEnd();
    }
}

// ---- WalletSpam pool control messages (see contracts/WalletSpam.tolk) ----

// Self-replicating pool deploy / top-up iteration (opcode kept from the FunC pool). One driver
// deploys a `chunk` of children then hands the remaining balance to the last child (next driver);
// the leaf/terminal cutoff is `subwalletId >= lastSubwallet`.
//   deployJetton: on  => each wallet also deploys its own prepaid jetton wallet (owner = itself);
//                 off => just walk the pool topping up wallet balances in order.
//   walletValue:  grams each pool wallet keeps (replaces the old hardcoded MIN_BALANCE).
//   jettonValue:  grams forwarded to deploy/top-up the wallet's jetton wallet.
//   chunk:        children per driver hop (1..253 — the contract asserts MAX_CHUNK=253, leaving
//                 headroom under TVM's 255-action cap for the reserve + own jetton deploy).
//                 Message-carried so it's a single tunable knob and testable with tiny pools
//                 (production ~200, tests ~3).
struct (0xba5e5d50) Reproduce {
    lastSubwallet: uint32
    fromSubwallet: uint32
    chunk: uint16
    deployJetton: bool
    walletValue: coins
    jettonValue: coins
}

// Linear chain teardown — NOT chunked. Every pool wallet hands its WHOLE balance to the very next wallet
// (subwallet+1) via CARRY_ALL_BALANCE|DESTROY (that carry-all is its own self-destruct), so balances roll
// down the chain to the terminal wallet, which sweeps the lot to `collector`. On the way, every wallet
// also destroys its OWN prepaid jetton wallet: it forwards the opaque `withdrawBody` (a WithdrawGrams
// cell, identical for all wallets — the recipient is baked into it, so it's built once off-chain and
// carried down instead of rebuilt on each wallet) to its jetton wallet with a fixed `jettonDestroyValue`,
// and the jetton wallet sweeps its GRAM to the recipient + self-destructs. jettonDestroyValue leaks out
// of the roll-up (it still reaches the recipient via the jetton path), so keep it small enough that the
// main chain never runs out of gas.
//   lastSubwallet:      id of the final wallet; it sweeps the roll-up to `collector` instead of forwarding.
//   collector:          where the final roll-up lands (usually == the recipient baked into withdrawBody).
//   jettonDestroyValue: fixed GRAM sent to each wallet's jetton wallet to fund its self-destruct.
//   withdrawBody:       opaque body forwarded verbatim to each wallet's own jetton wallet.
// The sender is authenticated as subwallet-1 (the previous link), so no fromSubwallet field is needed.
struct (0xde57c8a1) DestroyChain {
    lastSubwallet: uint32
    collector: address
    jettonDestroyValue: coins
    withdrawBody: cell
}
