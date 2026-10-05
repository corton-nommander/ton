// Fat-build gas probe (emulation): deploy one FatReproduce wallet and make it self-build N cells in a
// SINGLE FatBuild tx, for several N, printing the compute gas. Emulation runs the real TVM, so the gas
// COUNT matches mainnet/testnet (only the gas PRICE differs). Confirms a safe buildStep under the
// 1,000,000 basechain gas limit, and the fixed vs per-cell split. (Throwaway tool — delete after.)
// Run: acton script scripts/_fat_cost.tolk
import "@acton/build"
import "@acton/crypto"
import "@acton/io"
import "@acton/emulation/network"
import "@acton/emulation/scripts"
import "@acton/types/big_array"
import "@contracts/FatReproduce"
import "@wrappers/FatReproduce.gen"

const FAR_FUTURE = 0xFFFFFFFF

fun wallet0StateInit(pubkey: uint256): AutoDeployAddress {
    return {
        workchain: BASECHAIN,
        stateInit: ContractState {
            code: build("FatReproduce"),
            data: FatReproduceStorage {
                seqno: 0,
                subwalletId: 0,
                publicKey: pubkey,
                cellsBuilt: 0,
                batches: [],
            }.toCell(),
        },
    };
}

// A signed external that makes wallet 0 self-send ONE FatBuild(remaining=cells, buildStep=cells) — so it
// builds all `cells` in a single tx (no grow loop), which is what we want to measure.
fun buildSelfBuildExternal(kp: KeyPair, w0: address, seqno: int, cells: int): cell {
    val selfBuild = createMessage({
        bounce: BounceMode.NoBounce,
        value: grams("5"),
        dest: w0,
        body: FatBuild { remaining: cells, buildStep: min(cells, 180), buildValue: grams("5") },
    }).messageCell;
    val signed = beginCell()
        .storeUint(0, 32)
        .storeUint(FAR_FUTURE, 32)
        .storeUint(seqno, 32)
        .storeUint(SEND_MODE_PAY_FEES_SEPARATELY, 8)
        .storeRef(selfBuild)
        .endCell();
    val sig = crypto.rawSign(kp.privateKey, signed.hash());
    return beginCell().storeSlice(sig).storeSlice(signed.beginParse()).endCell();
}

fun probe(cells: int) {
    val deployer = scripts.wallet("deployer");
    val kp = crypto.generateKeyPair();
    val w0Init = wallet0StateInit(kp.publicKey);
    val w0Addr = w0Init.calculateAddress();

    net
        .send(deployer.address, createMessage({ bounce: false, value: grams("50"), dest: w0Init }))
        .waitForFirstTransaction();

    val res = net
        .sendExternal(
            net.createExternalMessage(w0Addr, buildSelfBuildExternal(kp, w0Addr, 0, cells)),
        )
        .waitForTrace()!;

    // The FatBuild tx is the one on wallet 0 that consumed the most gas (the external-processing root is
    // cheap). Print every tx's gas so the build tx is unambiguous.
    var maxGas = 0;
    var i = 0;
    while (i < res.size()) {
        val g = res.at(i).gasUsed;
        if (g > maxGas) {
            maxGas = g;
        }
        i += 1;
    }
    val built = FatReproduce.fromAddress(w0Addr).getCellsBuilt();
    println(
        "FATCOST cells={} built={} build_gas={} gas_per_cell={} txs={}",
        cells,
        built,
        maxGas,
        maxGas / cells,
        res.size(),
    );
}

fun main() {
    probe(50);
    probe(100);
    probe(300);
    probe(450);
    probe(600);
    println("DONE (1M gas is the ordinary basechain per-tx limit; keep build_gas well under it)");
}
