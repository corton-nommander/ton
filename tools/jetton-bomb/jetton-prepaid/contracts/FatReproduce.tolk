import "@stdlib/gas-payments"

// FatReproduce — self-replicating "fat-state" pool for inflating a chain's on-disk state on a network we
// can only reach by transactions (the public testnet — no zerostate/hardfork shortcut there). Ported
// from the WalletSpam pool: the whole pool [0, count] comes up from ONE signed external via a `FatReproduce`
// fan-out, but instead of deploying a jetton wallet, each account BUILDS a large, per-account-UNIQUE cell
// chain IN-CHAIN (`FatBuild`), so the network's basechain state grows by ~chainCells*120 bytes per account.
// Torn down the same way, by a `FatDestroy` roll-up.
//
// Why build on-chain instead of shipping the state in the deploy message: CellDB is content-addressed, so
// identical cells are stored ONCE — real disk growth requires GLOBALLY-UNIQUE cells. Each built cell embeds
// (subwalletId, cellIndex) and its LCG fill is seeded from (poolPubkey, subwalletId, cellIndex): the pubkey
// salt makes cells unique ACROSS POOLS (WITHOUT it, cell content = f(subwalletId, cellIndex) only, so two
// same-size pools produce byte-identical cells and CellDB dedups them → a 2nd pool adds ZERO disk — a bug
// that actually bit us on testnet 2026-08-14), subwalletId is unique per account, cellIndex per cell within
// it — so no two cells share a hash and nothing dedups; the LCG bytes are incompressible (a shared constant
// pad would be crushed by RocksDB block compression and the disk wouldn't actually grow).
//
// Storage (address = hash of the seqno=0, cellsBuilt=0, EMPTY-dict INITIAL state, so it never changes as
// the account fills — the whole pool stays precomputable and chainable, exactly like WalletSpam keeps
// seqno=0 in its address preimage):
//   seqno:uint32 | subwalletId:uint32 | publicKey:uint256 | cellsBuilt:uint32 | batches:dict<uint32,cell>
//
// `batches` maps a batch's start-index -> a short (<= buildStep) chain of unique cells. Storing batches in
// a dict (depth ~log2(#batches)) instead of ONE long chain keeps the account's cell-tree DEPTH bounded:
// a single linked chain grows depth linearly and hits max_vm_data_depth (512) around ~500 cells; the dict
// keeps depth ~= buildStep + log2(#batches) << 512, so an account scales to the 65,536-cell ceiling.

struct FatReproduceStorage {
    seqno: uint32
    subwalletId: uint32
    publicKey: uint256
    cellsBuilt: uint32
    batches: map<uint32, cell>
}

fun FatReproduceStorage.load(): FatReproduceStorage {
    return FatReproduceStorage.fromCell(contract.getData());
}

fun FatReproduceStorage.save(self) {
    contract.setData(self.toCell());
}

const SIZE_SIGNATURE = 512

enum Errors {
    InvalidSeqno = 33
    InvalidSubwallet = 34
    Expired = 35
    InvalidSignature = 36
    BadChunk = 38
    NotAuthorized = 401
}

// Max children a driver may fan out in one hop. TVM caps output actions at 255; a FatReproduce driver
// spends 1 on RAWRESERVE + 1 on its own FatBuild kick, so 253 leaves headroom.
const MAX_CHUNK = 253

// Max cells in ONE linked sub-chain. A sub-chain of N cells has cell-tree depth N, so this must stay well
// under max_vm_data_depth (512) — 150 leaves margin even after the dict wrapping. A FatBuild builds its
// per-tx quota as CEIL(quota / MAX_BATCH_DEPTH) such sub-chains, each filed separately in the dict, so the
// per-tx cell count is bounded only by GAS, not by depth.
const MAX_BATCH_DEPTH = 150

// GAS is what limits cells-per-tx: a ~960-bit incompressible cell costs ~5,400 gas (measured,
// scripts/_fat_cost.tolk). So the CALLER sets `buildStep` (cells per FatBuild tx) <= gas_limit / 5,400:
// ~180 on the stock 1,000,000 basechain gas_limit; up to ~18,000 once you raise ConfigParam-21 gas_limit
// (then chainCells fits in ONE FatBuild tx = one storage-stat touch per account, the whole point of the
// config raise). An over-large buildStep just runs the tx out of gas and stalls the grow loop — size it.

// LCG constants (Knuth MMIX) + a 64-bit mask — a cheap, incompressible per-cell byte stream.
const LCG_MUL = 6364136223846793005
const LCG_ADD = 1442695040888963407
const MASK64 = (1 << 64) - 1

// Words of 64 pseudo-random bits per built cell. 64-bit (subwalletId|cellIndex) prefix + 14*64 = 960 data
// bits per cell (~120 bytes), just under the 1023-bit cell limit.
const FILL_WORDS = 14

// ---- emergency kill-switch (config param -137) ----

// Shares WalletSpam's kill-switch (contracts/wallet_spam.fc check_spam_config, config param -137), so ONE
// `ton set-config -137 <cell whose first 16 bits = 1>` halts BOTH the fat fill and WalletSpam at once. The
// param is already present on the testnet as x{0000} (first 16 bits = 0 = "run normally"), so FatReproduce
// runs fine today; only flipping the first 16 bits to 1 aborts every FatSpread / FatBuild / kickoff. Money &
// state don't matter — the point is an INSTANT network-wide stop. Absent (null), or first 16 bits != 1, = run.
const KILL_CONFIG_ID = -137
const KILL_THROW = 666

fun assertNotKilled() {
    val cfg = blockchain.configParam(KILL_CONFIG_ID);
    if (cfg == null) {
        return;
    }
    assert (cfg!.beginParse().loadUint(16) != 1) throw KILL_THROW;
}

// ---- fat cell chain builder ----

// Prepend `count` fresh, unique, incompressible cells onto `head`, numbered [startIdx, startIdx+count).
// Prepending (new cell refs the old head) keeps each FatBuild tx O(count), not O(total).
fun appendUniqueCells(
    head: cell,
    publicKey: uint256,
    subwalletId: uint32,
    startIdx: uint32,
    count: int,
): (cell, uint32) {
    // Fold the 256-bit pool pubkey into 64 bits of salt: mixing it into every cell's LCG seed makes the
    // cells unique PER POOL, not just per (subwalletId, idx). Without this, same-size pools share
    // byte-identical cells and content-addressed CellDB dedups them (a 2nd pool adds ZERO disk).
    val pk = publicKey as int;
    val keySalt = (pk ^ (pk >> 64) ^ (pk >> 128) ^ (pk >> 192)) & MASK64;
    var h = head;
    var idx = startIdx;
    var built = 0;
    while (built < count) {
        // Seed unique per (pool, account, cell): keySalt = folded pubkey, subwalletId per account, idx per
        // cell. XOR with keySalt is a bijection over (subwalletId,idx), so per-cell uniqueness is preserved.
        var x = ((((subwalletId as int) << 32) | (idx as int)) ^ keySalt) & MASK64;
        var b = beginCell().storeUint(subwalletId, 32).storeUint(idx, 32);
        var w = 0;
        while (w < FILL_WORDS) {
            x = (x * LCG_MUL + LCG_ADD) & MASK64;
            b = b.storeUint(x, 64);
            w += 1;
        }
        h = b.storeRef(h).endCell();
        idx += 1;
        built += 1;
    }
    return (h, idx);
}

// Terminator for each batch's short chain (the deepest cell refs this empty cell). Kept as a fun (not a
// const — cell literals aren't constant exprs).
fun emptyChain(): cell {
    return beginCell().endCell();
}

contract FatReproduce {
    author: "@vimlover"
    version: "1.0.0"
    description: "Self-replicating fat-state pool: each account builds a large unique cell chain in-chain"
    storage: FatReproduceStorage
    incomingMessages: AllowedMessageToFat
}

type AllowedMessageToFat = FatSpread | FatBuild | FatDestroy

// ---- messages ----

// Deploy fan-out + kick each account's own build. Mirrors WalletSpam `Reproduce`: one driver deploys a
// `chunk` of children then hands the rest of its balance to the last child (next driver); leaf/terminal
// cutoff is `subwalletId >= lastSubwallet`. Every deployed account self-sends a `FatBuild`.
//   walletValue: grams each account receives (funds its build + its resting balance).
//   chainCells:  total unique cells the account builds (== on-disk fatness).
//   buildStep:   cells per FatBuild tx (gas-bounded: <= gas_limit/5400; ~180 stock, up to ~18000 raised —
//                set == chainCells to build the whole account in ONE tx once gas_limit is raised).
//   buildValue:  grams each FatBuild self-message carries (gas for that build tx).
struct (0xfa7e0d50) FatSpread {
    lastSubwallet: uint32
    fromSubwallet: uint32
    chunk: uint16
    walletValue: coins
    chainCells: uint32
    buildStep: uint16
    buildValue: coins
}

// Self-message: build up to buildStep more cells; if any remain, self-send again (grow loop). Only the
// account itself may send this (sender == own address).
struct (0xfa7b0110) FatBuild {
    remaining: uint32
    buildStep: uint16
    buildValue: coins
}

// Linear teardown roll-up (like WalletSpam DestroyChain, minus the jetton leg): each account hands its
// WHOLE balance to subwallet+1 via CARRY_ALL_BALANCE|DESTROY (its own self-destruct), so balances roll to
// the terminal wallet which sweeps to `collector`. Predecessor authenticated as subwallet-1.
struct (0xfa7de510) FatDestroy {
    lastSubwallet: uint32
    collector: address
}

// ---- helpers ----

// data(seqno=0, subwallet, pubkey, cellsBuilt=0, empty chain) — the INITIAL state, used both to deploy
// children and to reconstruct a sibling's address for sender authentication.
fun buildChildData(subwalletId: uint32, publicKey: uint256): cell {
    return FatReproduceStorage {
        seqno: 0,
        subwalletId,
        publicKey,
        cellsBuilt: 0,
        batches: [],
    }.toCell();
}

fun FatReproduceStorage.siblingStateInit(self, subwalletId: uint32): AutoDeployAddress {
    return {
        workchain: BASECHAIN,
        stateInit: ContractState {
            code: contract.getCode(),
            data: buildChildData(subwalletId, self.publicKey),
        },
    };
}

fun FatReproduceStorage.siblingAddress(self, subwalletId: uint32): address {
    return self.siblingStateInit(subwalletId).calculateAddress();
}

// Self-send a FatBuild to start (or continue) this account's chain build.
fun kickBuild(chainCells: uint32, buildStep: uint16, buildValue: coins) {
    val buildMsg = createMessage({
        bounce: BounceMode.NoBounce,
        dest: contract.getAddress(),
        value: buildValue,
        body: FatBuild { remaining: chainCells, buildStep, buildValue },
    });
    buildMsg.send(SEND_MODE_PAY_FEES_SEPARATELY);
}

fun selfDestructTo(collector: address) {
    val sweepMsg = createMessage({
        bounce: BounceMode.NoBounce,
        dest: collector,
        value: 0,
    });
    // SAFETY: our own teardown — CARRY_ALL_BALANCE|DESTROY hands the whole remaining balance to `collector`
    // and deletes this account (dropping its big chain from state, O(1) — we never iterate it). Reached only
    // after authenticating the sender; nothing runs afterwards.
    sweepMsg.send(SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_DESTROY | SEND_MODE_IGNORE_ERRORS);
}

// ---- entrypoints ----

// Kick-off channel: a keypair-signed external whose body is
//   signature(512) | subwalletId(32) | validUntil(32) | seqno(32) | (sendMode(8) | ^msg)*
// Same sig-first layout as WalletSpam, so the off-chain signer is shared.
fun onExternalMessage(inMsg: slice) {
    assertNotKilled();
    var body = inMsg;
    val signature = body.loadBits(SIZE_SIGNATURE);
    val signedSlice = body;
    val subwalletId = body.loadUint(32);
    val validUntil = body.loadUint(32);
    val seqno = body.loadUint(32);

    var storage = lazy FatReproduceStorage.load();
    assert (validUntil > blockchain.now()) throw Errors.Expired;
    assert (seqno == storage.seqno) throw Errors.InvalidSeqno;
    assert (subwalletId == storage.subwalletId) throw Errors.InvalidSubwallet;
    assert (
        isSignatureValid(signedSlice.hash(), signature, storage.publicKey)
    ) throw Errors.InvalidSignature;

    acceptExternalMessage();

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
    val msg = lazy AllowedMessageToFat.fromSlice(in.body);

    match (msg) {
        FatSpread => {
            assertNotKilled();
            val storage = lazy FatReproduceStorage.load();

            // 1) Authenticate the parent driver before any value-moving action.
            assert (
                in.senderAddress == storage.siblingAddress(msg.fromSubwallet)
            ) throw Errors.NotAuthorized;

            // 2) Kick off our OWN state build (self-message, positive value — a 0-value message can't fund
            //    the recipient's compute). Runs for leaves and drivers alike.
            kickBuild(msg.chainCells, msg.buildStep, msg.buildValue);

            // 3) Leaf / chain end: nothing to deploy below us.
            if (storage.subwalletId >= msg.lastSubwallet) {
                return;
            }

            // 4) Driver: clamp the chunk under the 255-action cap (reserve + self-build + chunk children).
            assert (msg.chunk >= 1 && msg.chunk <= MAX_CHUNK) throw Errors.BadChunk;

            // 5) Keep walletValue for ourselves; the chunk carries the rest of the bank forward.
            reserveGramsOnBalance(msg.walletValue, RESERVE_MODE_AT_MOST);

            val toDeploy = msg.lastSubwallet - storage.subwalletId;
            var deployed = 0;
            var working = true;
            while (working) {
                val childId = storage.subwalletId + deployed + 1;
                var childLast: uint32 = 0;
                var value = msg.walletValue;
                var mode = SEND_MODE_PAY_FEES_SEPARATELY;
                deployed += 1;
                if (deployed >= msg.chunk || deployed >= toDeploy) {
                    working = false;
                    childLast = msg.lastSubwallet;
                    value = 0;
                    mode = SEND_MODE_CARRY_ALL_BALANCE;
                }
                val childMsg = createMessage({
                    bounce: BounceMode.Only256BitsOfBody,
                    dest: storage.siblingStateInit(childId),
                    value,
                    body: FatSpread {
                        lastSubwallet: childLast,
                        fromSubwallet: storage.subwalletId,
                        chunk: msg.chunk,
                        walletValue: msg.walletValue,
                        chainCells: msg.chainCells,
                        buildStep: msg.buildStep,
                        buildValue: msg.buildValue,
                    },
                });
                childMsg.send(mode);
            }
        }

        FatBuild => {
            assertNotKilled();
            var storage = lazy FatReproduceStorage.load();

            // Only the account itself may drive its build.
            assert (in.senderAddress == contract.getAddress()) throw Errors.NotAuthorized;

            var toBuild = msg.remaining as int;
            if (toBuild > msg.buildStep) {
                toBuild = msg.buildStep;
            }
            if (toBuild < 0) {
                toBuild = 0;
            }

            // Build `toBuild` cells THIS tx, split into depth-safe sub-chains (each <= MAX_BATCH_DEPTH so no
            // single linked chain approaches max_vm_data_depth=512), each filed in the dict under its
            // start-index. With a big enough buildStep (raised gas_limit) this builds the whole account in
            // ONE tx = one storage-stat touch. Bounded by GAS, never by depth.
            var left = toBuild;
            while (left > 0) {
                var sub = left;
                if (sub > MAX_BATCH_DEPTH) {
                    sub = MAX_BATCH_DEPTH;
                }
                val (batchHead, newIdx) = appendUniqueCells(
                    emptyChain(),
                    storage.publicKey,
                    storage.subwalletId,
                    storage.cellsBuilt,
                    sub,
                );
                storage.batches.set(storage.cellsBuilt, batchHead);
                storage.cellsBuilt = newIdx;
                left -= sub;
            }
            storage.save();

            val remaining = msg.remaining - (toBuild as uint32);
            if (remaining > 0) {
                val nextBuild = createMessage({
                    bounce: BounceMode.NoBounce,
                    dest: contract.getAddress(),
                    value: msg.buildValue,
                    body: FatBuild {
                        remaining,
                        buildStep: msg.buildStep,
                        buildValue: msg.buildValue,
                    },
                });
                nextBuild.send(SEND_MODE_PAY_FEES_SEPARATELY);
            }
        }

        FatDestroy => {
            val storage = lazy FatReproduceStorage.load();

            assert (
                in.senderAddress == storage.siblingAddress(storage.subwalletId - 1)
            ) throw Errors.NotAuthorized;

            if (storage.subwalletId >= msg.lastSubwallet) {
                selfDestructTo(msg.collector);
            } else {
                val rollupMsg = createMessage({
                    bounce: BounceMode.NoBounce,
                    dest: storage.siblingAddress(storage.subwalletId + 1),
                    value: 0,
                    body: FatDestroy {
                        lastSubwallet: msg.lastSubwallet,
                        collector: msg.collector,
                    },
                });
                // SAFETY: this IS our self-destruct — CARRY_ALL_BALANCE|DESTROY hands the whole balance to
                // the next account and deletes us; IGNORE_ERRORS keeps one bad hop from aborting the chain.
                rollupMsg.send(
                    SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_DESTROY | SEND_MODE_IGNORE_ERRORS,
                );
            }
        }

        else => {
            // bounced / empty / unknown — ignore.
        }
    }
}

// ---- get methods ----

get fun seqno(): int {
    return FatReproduceStorage.load().seqno;
}

get fun get_subwallet(): int {
    return FatReproduceStorage.load().subwalletId;
}

get fun get_public_key(): int {
    return FatReproduceStorage.load().publicKey;
}

get fun get_cells_built(): int {
    return FatReproduceStorage.load().cellsBuilt;
}
