struct Storage {
    ownerAddress: address;
    isRunning: uint1;
    counter: uint64;
    limit: uint64;
}

fun Storage.load(): Storage {
    return Storage.fromCell(contract.getData());
}

fun Storage.save(self) {
    contract.setData(self.toCell());
}

const MIN_BALANCE = ton("0.05");

struct(0x5ce7c1d2) RunMsg {
    limit: uint64;
}
struct(0x07c32b3f) StopMsg {}

type AllowedMessage = RunMsg | StopMsg;

fun sendPing(queryId: uint64) {
    val emptyMasterchainAddr = address("-1:0000000000000000000000000000000000000000000000000000000000000009");
    
    val msg = createMessage({
        bounce: BounceMode.Only256BitsOfBody,
        value: 0,
        dest: emptyMasterchainAddr,
        body: beginCell()
            .storeUint(0xd53276db, 32)  // excesses op code
            .storeUint(queryId, 64)      // query_id (counter)
            .endCell()
    });
    reserveToncoinsOnBalance(MIN_BALANCE, 2);
    msg.send(128);
}

fun onInternalMessage(in: InMessage) {
    // accept empty messages (deploy)
    if (in.body.isEmpty()) {
        return;
    }
    
    var storage = Storage.load();
    
    val msg = lazy AllowedMessage.fromSlice(in.body);
    match (msg) {
        RunMsg => {
            // only owner can run
            assert (in.senderAddress == storage.ownerAddress) throw 401;
            
            if (storage.isRunning == 0) {
                storage.isRunning = 1;
                storage.counter = 0;
                storage.limit = msg.limit;
                storage.save();
                sendPing(storage.counter);
            }
        }
        StopMsg => {
            // anyone can stop
            if (storage.isRunning != 0) {
                storage.isRunning = 0;
                storage.save();
            }
        }
    }
}

fun onBouncedMessage(in: InMessageBounced) {
    var storage = Storage.load();
    
    // if still running, increment counter and check limit
    if (storage.isRunning != 0) {
        storage.counter += 1;
        
        // check if limit reached
        if (storage.counter >= storage.limit) {
            // stop automatically
            storage.isRunning = 0;
            storage.save();
        } else {
            // continue ping-pong
            storage.save();
            sendPing(storage.counter);
        }
    }
}

get fun isRunning(): int {
    val storage = Storage.load();
    return storage.isRunning;
}

get fun owner(): address {
    val storage = Storage.load();
    return storage.ownerAddress;
}

get fun counter(): int {
    val storage = Storage.load();
    return storage.counter;
}

get fun limit(): int {
    val storage = Storage.load();
    return storage.limit;
}

get fun getPingPongInfo(): (int, address, int, int) {
    val storage = Storage.load();
    return (storage.isRunning, storage.ownerAddress, storage.counter, storage.limit);
}