import "@stdlib/gas-payments"
import "jetton-utils"
import "messages"

// WalletSpam — self-replicating load-generator wallet, ported from contracts/wallet_spam.fc to Tolk.
//
// A pool of N of these comes up as a single self-propagating chain (`Reproduce`): each driver deploys
// a chunk of children and hands the rest of its balance forward, so the whole pool is stood up from
// one signed external to wallet 0. Every pool wallet owns exactly one prepaid jetton wallet that it
// deploys IN-CHAIN at reproduce time (no more external-message jetton deploys, nothing gets missed).
// The pool is torn down the same way, by a second chain (`DestroyChain`).
//
// Differences from the FunC original: storage carries the minter address + jetton-wallet code (so a
// wallet can build its own jetton wallet), the dead `spam_msg` ref and all Extra-Currency handling
// are gone, and the per-wallet balance / chunk size ride in the message instead of being hardcoded.
//
// Storage (address = hash of the seqno=0 initial state, so it never changes):
//   seqno:uint32 | subwalletId:uint32 | publicKey:uint256 | minterAddress:address | ^jettonWalletCode

struct WalletSpamStorage {
    seqno: uint32
    subwalletId: uint32
    publicKey: uint256
    minterAddress: address
    jettonWalletCode: cell
}

fun WalletSpamStorage.load(): WalletSpamStorage {
    return WalletSpamStorage.fromCell(contract.getData());
}

fun WalletSpamStorage.save(self) {
    contract.setData(self.toCell());
}

const SIZE_SIGNATURE = 512

// Throw codes (symbolic per lint E028; values kept close to the FunC original). WalletSpam keeps its
// OWN `Errors` — it is a different contract from the jetton package and doesn't import its errors.tolk.
enum Errors {
    InvalidSeqno = 33     // external seqno != stored seqno (stale / replay)
    InvalidSubwallet = 34 // external subwalletId != stored subwalletId
    Expired = 35          // external validUntil <= now
    InvalidSignature = 36 // external signature check failed
    BadChunk = 38         // Reproduce driver chunk out of 1..MAX_CHUNK
    NotAuthorized = 401   // internal sender is not the expected sibling (FunC used 401)
    InvalidC5 = 402       // signed-internal action list is malformed or contains non-send actions
}

// Max children a driver may fan out in one hop. TVM caps output actions at 255; a Reproduce driver
// spends 1 on RAWRESERVE + 1 on its own jetton deploy, so 253 leaves headroom. DestroyChain spends
// none of those but we clamp both paths to the same bound for simplicity.
const MAX_CHUNK = 253

contract WalletSpam {
    author: "@vimlover"
    version: "1.0.0"
    description: "Self-replicating load-generator wallet with in-chain prepaid jetton deploy"
    storage: WalletSpamStorage
    incomingMessages: AllowedMessageToWalletSpam
}

type AllowedMessageToWalletSpam = Reproduce | DestroyChain | InternalSignedRequest

// ---- helpers ----

// SAFETY: callers validate every node as action_send_msg before installing the raw OutList.
fun setTvmRegisterC5(actionList: cell): void
    asm "c5 POP"

// SAFETY: validation must inspect exotic action cells rather than fail before applying our own error.
fun OutActionsCell.beginParseAllowExotic(self): slice
    asm "XCTOS" "DROP"

fun OutActionsCell.verifyC5Actions(self): self {
    var count = 0;
    var cs = self.beginParseAllowExotic();
    do {
        val (nBits, nRefs) = cs.remainingBitsAndRefsCount();
        assert (nBits == 32 + 8 && nRefs == 2) throw Errors.InvalidC5;

        val action = lazy OutActionWithSendMessageOnly.fromSlice(
            cs,
            { throwIfOpcodeDoesNotMatch: Errors.InvalidC5 },
        );
        assert ((action.sendMode & SEND_MODE_IGNORE_ERRORS) != 0) throw Errors.InvalidC5;
        cs = action.prev.beginParseAllowExotic();
        count += 1;
    } while (!cs.isEmpty());

    assert (count <= 255) throw Errors.InvalidC5;
    return self;
}

// data(seqno=0, subwallet, pubkey, minter, jwCode) — used both to deploy children and to
// reconstruct a sibling's (seqno=0) address for sender authentication.
fun buildChildData(
    subwalletId: uint32,
    publicKey: uint256,
    minterAddress: address,
    jettonWalletCode: cell,
): cell {
    return WalletSpamStorage {
        seqno: 0,
        subwalletId,
        publicKey,
        minterAddress,
        jettonWalletCode,
    }.toCell();
}

// state_init that deploys pool wallet #subwallet (all siblings share this exact code).
fun WalletSpamStorage.siblingStateInit(self, subwalletId: uint32): AutoDeployAddress {
    return {
        workchain: BASECHAIN,
        stateInit: ContractState {
            code: contract.getCode(),
            data: buildChildData(
                subwalletId,
                self.publicKey,
                self.minterAddress,
                self.jettonWalletCode,
            ),
        },
    };
}

fun WalletSpamStorage.siblingAddress(self, subwalletId: uint32): address {
    return self.siblingStateInit(subwalletId).calculateAddress();
}

// Deploy (or top up) this wallet's own prepaid jetton wallet — owner = this WalletSpam.
fun WalletSpamStorage.deployOwnJettonWallet(self, jettonValue: coins) {
    val jwDest = calcDeployedJettonWallet(
        contract.getAddress(),
        self.minterAddress,
        self.jettonWalletCode,
    );
    val deployMsg = createMessage({
        bounce: BounceMode.NoBounce,
        dest: jwDest,
        value: jettonValue,
        body: TopUpGrams {},
    });
    deployMsg.send(SEND_MODE_PAY_FEES_SEPARATELY);
}

fun selfDestructTo(collector: address) {
    val sweepMsg = createMessage({
        bounce: BounceMode.NoBounce,
        dest: collector,
        value: 0,
    });
    // SAFETY: this is the pool wallet's own teardown — CARRY_ALL_BALANCE|DESTROY hands the entire
    // remaining balance to `collector` and deletes this wallet. Callers only reach here after
    // authenticating the sender, and nothing runs afterwards, so there is no state left to corrupt;
    // IGNORE_ERRORS keeps a single failed hop from aborting the rest of the teardown chain.
    sweepMsg.send(SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_DESTROY | SEND_MODE_IGNORE_ERRORS);
}

// ---- entrypoints ----

// Kick-off channel for the pool: a keypair-signed external whose body is
//   signature(512) | subwalletId(32) | validUntil(32) | seqno(32) | (sendMode(8) | ^msg)*
// The wrapped messages are executed verbatim; the pool is started by sending wallet 0 an external
// that wraps a Reproduce/DestroyChain internal aimed at wallet 1. Layout matches the FunC original
// (and the Go builder), so off-chain signers are unchanged.
fun onExternalMessage(inMsg: slice) {
    var body = inMsg;
    val signature = body.loadBits(SIZE_SIGNATURE);
    val signedSlice = body; // everything after the signature — this is what was signed (data + refs)
    val subwalletId = body.loadUint(32);
    val validUntil = body.loadUint(32);
    val seqno = body.loadUint(32);

    var storage = lazy WalletSpamStorage.load();
    assert (validUntil > blockchain.now()) throw Errors.Expired;
    assert (seqno == storage.seqno) throw Errors.InvalidSeqno;
    assert (subwalletId == storage.subwalletId) throw Errors.InvalidSubwallet;
    assert (
        isSignatureValid(signedSlice.hash(), signature, storage.publicKey)
    ) throw Errors.InvalidSignature;

    acceptExternalMessage();

    // Bump + commit the seqno before sending, so a replay can't reuse this external even if a wrapped
    // message later fails.
    storage.seqno += 1;
    storage.save();
    commitContractDataAndActions();

    while (body.remainingRefsCount() > 0) {
        val mode = body.loadUint(8);
        sendRawMessage(body.loadRef(), mode);
    }
}

@on_bounced_policy("manual")
fun onInternalMessage(in: InMessage) {
    val msg = lazy AllowedMessageToWalletSpam.fromSlice(in.body);

    match (msg) {
        InternalSignedRequest => {
            // W5 `sint`: opcode + walletId + validUntil + seqno + two action-presence bits + signature.
            // Random deposits with a short body or bad signature are accepted silently.
            if (in.body.remainingBitsCount() < SIZE_SIGNATURE + 130) {
                return;
            }

            val signature = in.body.getLastBits(SIZE_SIGNATURE);
            val signedSlice = in.body.removeLastBits(SIZE_SIGNATURE);
            var storage = lazy WalletSpamStorage.load();
            if (!isSignatureValid(signedSlice.hash(), signature, storage.publicKey)) {
                return;
            }

            assert (msg.seqno == storage.seqno) throw Errors.InvalidSeqno;
            assert (msg.walletId == storage.subwalletId) throw Errors.InvalidSubwallet;
            assert (msg.validUntil > blockchain.now()) throw Errors.Expired;
            assert (!msg.hasExtraActions) throw Errors.InvalidC5;

            // Validate before consuming the shared counter, then commit the counter and c5 together so
            // authenticated requests are at-most-once even if later execution fails.
            var actions = msg.outActions;
            if (actions != null) {
                actions = actions.verifyC5Actions();
            }
            storage.seqno += 1;
            storage.save();

            if (actions != null) {
                setTvmRegisterC5(actions);
            }
            commitContractDataAndActions();
        }

        Reproduce => {
            val storage = lazy WalletSpamStorage.load();

            // 1) Authenticate the parent driver BEFORE any value-moving action. A genuine leaf/driver
            //    receives fromSubwallet = its driver's id, so its real sender equals
            //    siblingAddress(fromSubwallet); only unauthenticated callers are rejected. This stops a
            //    stranger from making us spend an attacker-chosen jettonValue out of our own balance.
            assert (
                in.senderAddress == storage.siblingAddress(msg.fromSubwallet)
            ) throw Errors.NotAuthorized;

            // 2) Every pool wallet deploys/tops-up its own prepaid jetton wallet (the analog of the
            //    old spam_msg send) — runs for leaves and drivers alike, before the leaf cutoff.
            if (msg.deployJetton) {
                storage.deployOwnJettonWallet(msg.jettonValue);
            }

            // 3) Leaf / chain end: nothing more to deploy below this wallet.
            if (storage.subwalletId >= msg.lastSubwallet) {
                return;
            }

            // 4) Driver: clamp the chunk so we can't overrun the 255-action limit and abort the hop
            //    (which would silently drop the whole descendant sub-pool).
            assert (msg.chunk >= 1 && msg.chunk <= MAX_CHUNK) throw Errors.BadChunk;

            // 5) Keep our footprint for ourselves; the chunk carries the rest of the balance forward.
            //    A leaf pays its own jetton deploy out of the walletValue it receives, so it rests at
            //    walletValue - jettonValue - fee. A driver funds its jetton from the incoming bank
            //    BEFORE this reserve, so reserving the full walletValue would leave every driver richer
            //    than every leaf (0.2 vs 0.149). Subtract the jetton we just funded so all pool wallets
            //    rest at the same GRAM. (Guarded so a degenerate jettonValue >= walletValue can't
            //    underflow the reserve — then we just keep the full walletValue, the old behavior.)
            var reserveAmount = msg.walletValue;
            if (msg.deployJetton && reserveAmount > msg.jettonValue) {
                reserveAmount -= msg.jettonValue;
            }
            reserveGramsOnBalance(reserveAmount, RESERVE_MODE_AT_MOST);

            val toDeploy = msg.lastSubwallet - storage.subwalletId;
            var deployed = 0;
            var working = true;
            while (working) {
                val childId = storage.subwalletId + deployed + 1;
                var childLast: uint32 = 0; // leaf by default (childId >= 0 stops it immediately)
                var value = msg.walletValue;
                var mode = SEND_MODE_PAY_FEES_SEPARATELY;
                deployed += 1;
                if (deployed >= msg.chunk || deployed >= toDeploy) {
                    working = false;
                    childLast = msg.lastSubwallet; // last child = next driver (or terminal leaf)
                    value = 0;
                    mode = SEND_MODE_CARRY_ALL_BALANCE;
                }
                val childMsg = createMessage({
                    bounce: BounceMode.Only256BitsOfBody,
                    dest: storage.siblingStateInit(childId),
                    value,
                    body: Reproduce {
                        lastSubwallet: childLast,
                        fromSubwallet: storage.subwalletId,
                        chunk: msg.chunk,
                        deployJetton: msg.deployJetton,
                        walletValue: msg.walletValue,
                        jettonValue: msg.jettonValue,
                    },
                });
                // `mode` is PAY_FEES_SEPARATELY for the non-last children and CARRY_ALL_BALANCE for the
                // last (the next driver), which hands the rest of the balance down the reproduce chain.
                childMsg.send(mode);
            }
        }

        DestroyChain => {
            val storage = lazy WalletSpamStorage.load();

            // Linear chain: our predecessor is always subwallet-1. (Wallet 0 is driven by the external,
            // so it never lands here; any internal recipient has subwalletId >= 1.)
            assert (
                in.senderAddress == storage.siblingAddress(storage.subwalletId - 1)
            ) throw Errors.NotAuthorized;

            // 1) Destroy our OWN prepaid jetton wallet: forward the opaque withdraw body to it, funded
            //    with the fixed jettonDestroyValue (the jetton wallet sweeps its GRAM to the recipient
            //    baked into withdrawBody, then self-destructs). Registered FIRST so the CARRY_ALL_BALANCE
            //    roll-up below only carries what's left after this leaks out.
            val jettonDestroyMsg = createMessage({
                bounce: BounceMode.NoBounce,
                dest: calcAddressOfJettonWallet(
                    contract.getAddress(),
                    storage.minterAddress,
                    storage.jettonWalletCode,
                ),
                value: msg.jettonDestroyValue,
                body: msg.withdrawBody,
            });
            jettonDestroyMsg.send(SEND_MODE_PAY_FEES_SEPARATELY);

            // 2) Hand our whole remaining balance onward and self-destruct. Terminal wallet -> collector;
            //    otherwise -> the very next wallet, carrying the same DestroyChain (withdrawBody rides on).
            if (storage.subwalletId >= msg.lastSubwallet) {
                selfDestructTo(msg.collector);
            } else {
                val rollupMsg = createMessage({
                    bounce: BounceMode.NoBounce,
                    dest: storage.siblingAddress(storage.subwalletId + 1),
                    value: 0,
                    body: DestroyChain {
                        lastSubwallet: msg.lastSubwallet,
                        collector: msg.collector,
                        jettonDestroyValue: msg.jettonDestroyValue,
                        withdrawBody: msg.withdrawBody,
                    },
                });
                // SAFETY: this IS our self-destruct — CARRY_ALL_BALANCE|DESTROY hands the whole remaining
                // balance to the next wallet in the chain and deletes us. The jetton-destroy message above
                // is already queued, and no code runs after; IGNORE_ERRORS stops one bad hop from aborting
                // the teardown.
                rollupMsg.send(
                    SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_DESTROY | SEND_MODE_IGNORE_ERRORS,
                );
            }
        }

        else => {
            // bounced / empty / unknown opcode — ignore (FunC skipped bounced + non-reproduce ops).
        }
    }
}

// ---- get methods (match the FunC originals) ----

get fun seqno(): int {
    return WalletSpamStorage.load().seqno;
}

get fun get_subwallet(): int {
    return WalletSpamStorage.load().subwalletId;
}

get fun get_public_key(): int {
    return WalletSpamStorage.load().publicKey;
}

get fun get_minter(): address {
    return WalletSpamStorage.load().minterAddress;
}
