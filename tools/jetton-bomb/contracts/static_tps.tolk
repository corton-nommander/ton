struct Storage {
    ownerAddress: address;
    isRunning: uint1;
    targetTps: uint32;
    windowStart: uint32;
    windowTxCount: uint64;
    totalTxCount: uint64;
    activeEpoch: uint32;
}

fun Storage.load(): Storage {
    return Storage.fromCell(contract.getData());
}

fun Storage.save(self) {
    contract.setData(self.toCell());
}

const MIN_BALANCE = ton("10");
const WORK_MSG_VALUE = ton("0.003");
// Masterchain forwarding/compute prices are higher. The attached value returns
// to this same account; enough float prevents mode 2 silently dropping work.
const MASTERCHAIN_WORK_MSG_VALUE = ton("1");
const WINDOW_SIZE: int = 30;
const MAX_WINDOW_TX: int = 100000; // max tx per window for safety
const MAX_EPOCH: int = 4294967295;

struct(0x5ce7c1d2) RunMsg {
    targetTps: uint32;
}
struct(0x07c32b3f) StopMsg {}
struct(0x10adbeef) WorkMsg {
    epoch: uint32;
}

type AllowedMessage = WorkMsg | RunMsg | StopMsg;

// bounces off a nonexistent masterchain address to wake us up
fun sendPing(queryId: uint64) {
    val emptyMasterchainAddr = address("-1:0000000000000000000000000000000000000000000000000000000000000003");

    val msg = createMessage({
        bounce: BounceMode.Only256BitsOfBody,
        value: ton("1"),
        dest: emptyMasterchainAddr,
        body: beginCell()
            .storeUint(0xd53276db, 32)
            .storeUint(queryId, 64)
            .endCell()
    });
    msg.send(3);
}

fun nextEpoch(epoch: int): int {
    if (epoch >= MAX_EPOCH) {
        return 0;
    }
    return epoch + 1;
}

// sends work message to self - the message itself is the load
fun sendContinue(selfAddr: address, epoch: int) {
    val msg = createMessage({
        bounce: BounceMode.NoBounce,
        value: selfAddr.getWorkchain() == MASTERCHAIN ? MASTERCHAIN_WORK_MSG_VALUE : WORK_MSG_VALUE,
        dest: selfAddr,
        body: WorkMsg { epoch }
    });
    msg.send(2);
}

fun onInternalMessage(in: InMessage) {
    if (in.body.isEmpty()) {
        return;
    }

    var storage = Storage.load();

    val msg = lazy AllowedMessage.fromSlice(in.body);
    match (msg) {
        WorkMsg => {
            // only process messages from self
            if (in.senderAddress != contract.getAddress()) {
                return;
            }

            if (storage.isRunning == 0) {
                return;
            }

            // ignore messages from old epochs
            if (msg.epoch != storage.activeEpoch) {
                return;
            }

            reserveToncoinsOnBalance(MIN_BALANCE, 2);

            // calculate target for this window
            var targetTx: int = storage.targetTps * WINDOW_SIZE;
            if (targetTx > MAX_WINDOW_TX) {
                targetTx = MAX_WINDOW_TX;
            }
            
            if (storage.windowTxCount < targetTx) {
                // this message itself is the payload
                storage.windowTxCount += 1;
                storage.totalTxCount += 1;
                storage.save();
                
                // if more work needed, send next message to self
                if (storage.windowTxCount < targetTx) {
                    sendContinue(contract.getAddress(), storage.activeEpoch);
                }
            }
        }
        RunMsg => {
            assert (in.senderAddress == storage.ownerAddress) throw 401;

            storage.targetTps = msg.targetTps;
            storage.windowStart = blockchain.now() - WINDOW_SIZE + 5;
            storage.windowTxCount = 0;
            storage.activeEpoch = nextEpoch(storage.activeEpoch);

            if (storage.isRunning == 0) {
                storage.isRunning = 1;
                storage.save();
                reserveToncoinsOnBalance(MIN_BALANCE, 2);
                sendPing(storage.totalTxCount);
            } else {
                // hot-update target rate and invalidate old children
                storage.save();
            }
        }
        StopMsg => {
            storage.isRunning = 0;
            storage.activeEpoch = nextEpoch(storage.activeEpoch);
            storage.save();
        }
    }
}

fun onBouncedMessage(in: InMessageBounced) {
    var storage = Storage.load();

    if (storage.isRunning == 0) {
        return;
    }

    val currentTime: int = blockchain.now();
    var elapsed: int = currentTime - storage.windowStart;
    reserveToncoinsOnBalance(MIN_BALANCE, 2);

    // slide window forward when expired
    if (elapsed >= WINDOW_SIZE) {
        storage.windowStart = currentTime;
        storage.windowTxCount = 0;

        // start new epoch to invalidate old chain
        storage.activeEpoch = nextEpoch(storage.activeEpoch);
        
        storage.save();

        // kick off the chain with first message
        val targetTx: int = storage.targetTps * WINDOW_SIZE;
        if (targetTx > 0) {
            sendContinue(contract.getAddress(), storage.activeEpoch);
        }
    }

    sendPing(storage.totalTxCount);
}

get fun isRunning(): int {
    val storage = Storage.load();
    return storage.isRunning;
}

get fun owner(): address {
    val storage = Storage.load();
    return storage.ownerAddress;
}

get fun targetTps(): int {
    val storage = Storage.load();
    return storage.targetTps;
}

get fun windowTxCount(): int {
    val storage = Storage.load();
    return storage.windowTxCount;
}

get fun totalTxCount(): int {
    val storage = Storage.load();
    return storage.totalTxCount;
}

get fun activeEpoch(): int {
    val storage = Storage.load();
    return storage.activeEpoch;
}

get fun maxTps(): int {
    return MAX_WINDOW_TX / WINDOW_SIZE;
}

get fun getInfo(): (int, address, int, int, int, int, int) {
    val storage = Storage.load();
    return (
        storage.isRunning,
        storage.ownerAddress,
        storage.targetTps,
        storage.windowStart,
        storage.windowTxCount,
        storage.totalTxCount,
        storage.activeEpoch
    );
}
