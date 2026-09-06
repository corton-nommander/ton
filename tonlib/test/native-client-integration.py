#!/usr/bin/env python3
"""Small, state-changing tonlib JSON compatibility test against a local funded chain.

Requires an idle local validator and exclusive use of source-N/source-(N+1).
Imports raw fixture keys into an in-memory tonlib keystore. No key bytes, imported
key secrets, signed message bodies, or request payloads are written to the report.
This is a correctness test, not a load/capacity/TPS benchmark.
"""
import argparse
import base64
import ctypes
import hashlib
import json
from pathlib import Path
import sys
import time


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def b64(data):
    return base64.b64encode(data).decode("ascii")


def unb64(text):
    return base64.b64decode(text, validate=True)


def sha_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def crc32c(data):
    value = 0xffffffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x82f63b78 if value & 1 else 0)
    return (value ^ 0xffffffff).to_bytes(4, "little")


def parse_native_boc(blob):
    """Independent bounded ordinary-cell BOC parser/hash oracle for NTXF/NTRN."""
    require(blob[:4] == bytes.fromhex("b5ee9c72"), "Unsupported BOC magic")
    flags, offset_size = blob[4:6]
    size = flags & 7
    indexed, checksum = bool(flags & 0x80), bool(flags & 0x40)
    require(1 <= size <= 4 and 1 <= offset_size <= 8 and not flags & 0x38,
            "Unexpected BOC flags")
    position = 6

    def read_int(width):
        nonlocal position
        require(position + width <= len(blob), "Truncated BOC integer")
        value = int.from_bytes(blob[position:position + width], "big")
        position += width
        return value

    cells_count, roots_count, absent_count = (read_int(size) for _ in range(3))
    cells_size = read_int(offset_size)
    require(0 < cells_count <= 128 and roots_count == 1 and absent_count == 0,
            "Unexpected native BOC population")
    root = read_int(size)
    require(root < cells_count, "BOC root out of bounds")
    if indexed:
        position += cells_count * offset_size
    end = position + cells_size
    require(end + (4 if checksum else 0) == len(blob), "BOC size mismatch")
    if checksum:
        require(crc32c(blob[:-4]) == blob[-4:], "Invalid BOC CRC32C")
    cells = []
    for index in range(cells_count):
        require(position + 2 <= end, "Truncated BOC cell")
        d1, d2 = blob[position:position + 2]
        position += 2
        refs = d1 & 7
        require(d1 < 8 and refs <= 4, "Expected level-zero ordinary native cells")
        byte_count = (d2 + 1) // 2
        data_offset = position
        require(position + byte_count + refs * size <= end, "Truncated BOC payload")
        data = blob[position:position + byte_count]
        position += byte_count
        references = [read_int(size) for _ in range(refs)]
        require(all(index < child < cells_count for child in references), "Unexpected BOC topology")
        cells.append({"descriptor": bytes((d1, d2)), "data": data, "offset": data_offset,
                      "refs": references})
    require(position == end, "Trailing BOC cell data")
    for index in reversed(range(cells_count)):
        cell = cells[index]
        children = [cells[child] for child in cell["refs"]]
        cell["depth"] = max((child["depth"] + 1 for child in children), default=0)
        cell["hash"] = hashlib.sha256(
            cell["descriptor"] + cell["data"]
            + b"".join(child["depth"].to_bytes(2, "big") for child in children)
            + b"".join(child["hash"] for child in children)).digest()
    require(cells[root]["data"][:4] in (b"NTXF", b"NTRN"), "Expected native parent")
    return cells[root]["hash"], cells, checksum


def corrupt_signature(blob):
    _, cells, checksum = parse_native_boc(blob)
    signatures = [cell for cell in cells if cell["descriptor"] == bytes((0, 128))]
    require(len(signatures) == 1, "Expected one 512-bit signature cell")
    changed = bytearray(blob)
    changed[signatures[0]["offset"]] ^= 1
    if checksum:
        changed[-4:] = crc32c(changed[:-4])
    result = bytes(changed)
    # Validate the rebuilt BOC and compute the actual hash of the invalid parent.
    parse_native_boc(result)
    return result


class TonlibError(Exception):
    def __init__(self, query_type, response):
        super().__init__(f"{query_type}: error {response.get('code')}: {response.get('message')}")
        self.response = response


class Tonlib:
    def __init__(self, library, timeout, events):
        self.lib = ctypes.CDLL(str(library))
        self.lib.tonlib_client_json_create.argtypes = []
        self.lib.tonlib_client_json_create.restype = ctypes.c_void_p
        self.lib.tonlib_client_json_destroy.argtypes = [ctypes.c_void_p]
        self.lib.tonlib_client_json_destroy.restype = None
        self.lib.tonlib_client_json_send.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self.lib.tonlib_client_json_send.restype = None
        self.lib.tonlib_client_json_receive.argtypes = [ctypes.c_void_p, ctypes.c_double]
        self.lib.tonlib_client_json_receive.restype = ctypes.c_char_p
        self.lib.tonlib_client_json_execute.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self.lib.tonlib_client_json_execute.restype = ctypes.c_char_p
        self.client = self.lib.tonlib_client_json_create()
        require(self.client, "tonlib client creation failed")
        self.sequence = 0
        self.timeout = timeout
        self.events = events
        self.lib.tonlib_client_json_execute(
            self.client, b'{"@type":"setLogVerbosityLevel","new_verbosity_level":0}')

    def call(self, request, expect_error=False):
        self.sequence += 1
        request = dict(request, **{"@extra": str(self.sequence)})
        started = time.monotonic()
        self.lib.tonlib_client_json_send(self.client, json.dumps(request).encode())
        while time.monotonic() - started < self.timeout:
            data = self.lib.tonlib_client_json_receive(self.client, 0.25)
            if not data:
                continue
            response = json.loads(data)
            if response.get("@extra") != str(self.sequence):
                continue
            failed = response.get("@type") == "error"
            self.events.append({"query": request["@type"], "response_type": response.get("@type"),
                                "elapsed_seconds": time.monotonic() - started,
                                "expected_error": expect_error, "error_code": response.get("code") if failed else None})
            if failed and not expect_error:
                raise TonlibError(request["@type"], response)
            require(failed == expect_error, f"Unexpected success for {request['@type']}")
            return response
        raise TimeoutError(f"{request['@type']}: request deadline {self.timeout}s exceeded")

    def close(self):
        if self.client:
            self.lib.tonlib_client_json_destroy(self.client)
            self.client = None


def account(address):
    return {"@type": "accountAddress", "account_address": address}


def state(client, address):
    result = client.call({"@type": "native.getAccountState", "account_address": account(address)})
    require(result.get("@type") == "native.fullAccountState", "Wrong native full account variant")
    for field in ("balance", "nonce"):
        value = result[field]
        require(isinstance(value, str) and value.isascii() and value.isdecimal()
                and 0 <= int(value) <= 2**64 - 1, f"Native {field} did not preserve decimal uint64")
    return result


def wait_state(client, address, nonce, balance, deadline):
    last = None
    while time.monotonic() < deadline:
        last = state(client, address)
        require(int(last["nonce"]) <= nonce, "Unexpected concurrent source nonce consumption")
        if int(last["nonce"]) == nonce and int(last["balance"]) == balance:
            return last
        time.sleep(0.25)
    raise TimeoutError(f"Canonical account delta missing for {address}; expected nonce={nonce}, balance={balance}; "
                       f"last nonce={last['nonce'] if last else None}, balance={last['balance'] if last else None}")


def run(args, report):
    config_text = args.config.read_text()
    config = json.loads(config_text)
    domain = unb64(config["validator"]["zero_state"]["root_hash"])
    require(len(domain) == 32, "Config zerostate root must have 32 bytes")
    report["inputs"] = {"library": str(args.library), "library_sha256": sha_file(args.library),
                        "config": str(args.config), "config_sha256": sha_file(args.config),
                        "zerostate_root_hash": b64(domain), "source_indices": [args.source_index, args.source_index + 1],
                        "payment_lane_depth": args.lane_depth, "logical_count_per_run": args.logical_count,
                        "amount_per_transfer": str(args.amount), "fee_per_transfer": str(args.fee)}
    client = Tonlib(args.library, args.timeout, report["requests"])
    try:
        client.call({"@type": "init", "options": {"@type": "options", "config": {
            "@type": "config", "config": config_text, "blockchain_name": "",
            "use_callbacks_for_network": False, "ignore_cache": True},
            "keystore_type": {"@type": "keyStoreTypeInMemory"}}})
        wallets = []
        for index in (args.source_index, args.source_index + 1):
            public = (args.wallet_dir / f"source-{index}.pub").read_bytes()
            destination = (args.wallet_dir / f"dest-{index}.pub").read_bytes()
            private = (args.wallet_dir / f"source-{index}.pk").read_bytes()
            require(len(public) == len(destination) == len(private) == 32, "Fixture key must contain exactly 32 bytes")
            require(public != destination, "Integration test requires distinct source and destination")
            if args.lane_depth:
                require(int.from_bytes(public, "big") >> (256 - args.lane_depth)
                        == int.from_bytes(destination, "big") >> (256 - args.lane_depth), "Fixture lane mismatch")
            key = client.call({"@type": "importUnencryptedKey", "local_password": "",
                               "exported_unencrypted_key": {"@type": "exportedUnencryptedKey", "data": b64(private)}})
            del private
            native_address = client.call({"@type": "native.getAccountAddress", "public_key": key["public_key"]})
            decoded = client.call({"@type": "unpackAccountAddress", "account_address": native_address["account_address"]})
            require(decoded["workchain_id"] == 0 and unb64(decoded["addr"]) == public, "Native address differs from key")
            source = "0:" + public.hex()
            target = "0:" + destination.hex()
            wallet = {"source": source, "destination": target,
                      "input_key": {"@type": "inputKeyRegular", "key": {"@type": "key", "public_key": key["public_key"],
                                    "secret": key["secret"]}, "local_password": ""},
                      "before": state(client, source), "destination_before": state(client, target)}
            require(int(wallet["before"]["balance"]) >= 3 * args.logical_count * (args.amount + args.fee),
                    "Funded source balance is too small")
            generic = client.call({"@type": "getAccountState", "account_address": account(source)})
            require(generic["account_state"]["@type"] == "native.accountState", "Generic account path lost native variant")
            require(generic["account_state"]["nonce"] == wallet["before"]["nonce"], "Generic native nonce mismatch")
            require(generic["account_state"]["balance"] == wallet["before"]["balance"]
                    and generic["account_state"]["flags"] == wallet["before"]["flags"], "Generic native fields mismatch")
            raw = client.call({"@type": "raw.getAccountState", "account_address": account(source)})
            require(int(raw["balance"]) == int(wallet["before"]["balance"]), "Legacy raw native balance mismatch")
            require(raw["code"] == "" and raw["data"] == "", "Native raw state unexpectedly synthesized wallet code/data")
            wallets.append(wallet)
        require(len({w["source"] for w in wallets} | {w["destination"] for w in wallets}) == 4,
                "Integration fixture accounts must be independent")

        report["accounts_before"] = [
            {"source": w["source"], "destination": w["destination"],
             "before": w["before"], "destination_before": w["destination_before"]} for w in wallets]
        report["submission_attempts"] = []

        def create(wallet, nonce, **overrides):
            request = {"@type": "native.createTransferRun", "input_key": wallet["input_key"],
                       "outputs": [{"@type": "native.transferOutput", "destination": account(wallet["destination"]),
                                    "amount": str(args.amount), "fee": str(args.fee)}] * args.logical_count,
                       "first_nonce": str(nonce), "valid_until": int(time.time()) + 600,
                       "chain_domain": b64(domain), "payment_lane_depth": args.lane_depth}
            request.update(overrides)
            return request

        first_nonce = int(wallets[0]["before"]["nonce"])
        wrong_domain = bytes([domain[0] ^ 1]) + domain[1:]
        client.call(create(wallets[0], first_nonce, chain_domain=b64(wrong_domain)), expect_error=True)
        client.call(create(wallets[0], first_nonce, first_nonce=str(2**64)), expect_error=True)
        client.call(create(wallets[0], first_nonce, valid_until=int(time.time()) - 1), expect_error=True)
        client.call({"@type": "raw.sendMessageBatch", "bodies": []}, expect_error=True)

        wrong_workchain = {"@type": "native.transferOutput", "destination": account("-1:" + wallets[0]["destination"][2:]),
                           "amount": str(args.amount), "fee": str(args.fee)}
        client.call(create(wallets[0], first_nonce, outputs=[wrong_workchain]), expect_error=True)
        if args.lane_depth:
            target = bytearray.fromhex(wallets[0]["destination"][2:])
            target[0] ^= 0x80
            wrong_lane = dict(wrong_workchain, destination=account("0:" + target.hex()))
            client.call(create(wallets[0], first_nonce, outputs=[wrong_lane]), expect_error=True)

        # Scalar construction remains available for native clients even when
        # this chain's admission policy requires signed runs on the wire.
        scalar = client.call({"@type": "native.createTransfer", "input_key": wallets[0]["input_key"],
                              "destination": account(wallets[0]["destination"]), "amount": str(args.amount),
                              "fee": str(args.fee), "nonce": str(first_nonce), "valid_until": int(time.time()) + 600,
                              "chain_domain": b64(domain), "payment_lane_depth": args.lane_depth})
        scalar_hash, scalar_cells, _ = parse_native_boc(unb64(scalar["body"]))
        require(unb64(scalar["hash"]) == scalar_hash and scalar_cells[0]["data"][:4] == b"NTXF",
                "Scalar native construction/hash mismatch")
        report["scalar_constructed_hash"] = b64(scalar_hash)
        count = args.logical_count
        debit = count * (args.amount + args.fee)
        raw_message = client.call(create(wallets[0], first_nonce))
        raw_hash = parse_native_boc(unb64(raw_message["body"]))[0]
        require(unb64(raw_message["hash"]) == raw_hash, "Raw-message signed parent hash mismatch")
        report["submission_attempts"].append({"api": "raw.sendMessage", "hashes": [b64(raw_hash)]})
        raw_send_result = client.call({"@type": "raw.sendMessage", "body": raw_message["body"]})
        report["raw_send_result"] = raw_send_result
        require(raw_send_result.get("@type") == "ok", "raw.sendMessage did not return Ok")
        after_raw = wait_state(client, wallets[0]["source"], first_nonce + count,
                               int(wallets[0]["before"]["balance"]) - debit, time.monotonic() + args.canonical_timeout)
        report["after_raw_source"] = after_raw
        single = client.call(create(wallets[0], first_nonce + count))
        single_hash = parse_native_boc(unb64(single["body"]))[0]
        require(unb64(single["hash"]) == single_hash, "Signed parent hash differs from independent cell hash")
        report["submission_attempts"].append({"api": "raw.sendMessageReturnHash", "hashes": [b64(single_hash)]})
        single_result = client.call({"@type": "raw.sendMessageReturnHash", "body": single["body"]})
        report["single_send_result"] = single_result
        require(unb64(single_result["hash"]) == single_hash and unb64(single_result["hash_norm"]) == single_hash,
                "Single-message exact native hash mismatch")
        after_single = wait_state(client, wallets[0]["source"], first_nonce + 2 * count,
                                  int(wallets[0]["before"]["balance"]) - 2 * debit, time.monotonic() + args.canonical_timeout)
        report["after_single_source"] = after_single
        first = client.call(create(wallets[0], first_nonce + 2 * count))
        invalid_original = client.call(create(wallets[0], first_nonce + 3 * count))
        invalid_body = corrupt_signature(unb64(invalid_original["body"]))
        last = client.call(create(wallets[1], int(wallets[1]["before"]["nonce"])))
        bodies = [unb64(first["body"]), invalid_body, unb64(last["body"])]
        expected_hashes = [parse_native_boc(body)[0] for body in bodies]
        require(unb64(first["hash"]) == expected_hashes[0] and unb64(last["hash"]) == expected_hashes[2],
                "Batch signed parent hash mismatch")
        report["submission_attempts"].append({"api": "raw.sendMessageBatch", "hashes": [b64(h) for h in expected_hashes]})
        batch = client.call({"@type": "raw.sendMessageBatch", "bodies": [b64(body) for body in bodies]})
        report["batch_result"] = batch
        statuses = batch["results"]
        require(len(statuses) == 3, "Partial batch result count mismatch")
        require([s["status"] for s in statuses] == [1, 0, 1], "Batch ordered partial acceptance mismatch")
        for index, result in enumerate(statuses):
            require(unb64(result["hash"]) == expected_hashes[index]
                    and unb64(result["hash_norm"]) == expected_hashes[index], "Batch item hash/order mismatch")
            if index != 1:
                require(result["code"] == 0 and result["message"] == "", "Successful batch item contains an error")
        require(statuses[1]["code"] != 0 and "signature" in statuses[1]["message"].lower(),
                "Invalid parent was not rejected for signature validation")
        finals = []
        for index, wallet in enumerate(wallets):
            runs = 3 if index == 0 else 1
            deadline = time.monotonic() + args.canonical_timeout
            source_final = wait_state(client, wallet["source"], int(wallet["before"]["nonce"]) + runs * count,
                                      int(wallet["before"]["balance"]) - runs * debit, deadline)
            destination_final = wait_state(client, wallet["destination"], int(wallet["destination_before"]["nonce"]),
                                           int(wallet["destination_before"]["balance"]) + runs * count * args.amount, deadline)
            finals.append({"source": wallet["source"], "destination": wallet["destination"],
                           "before": wallet["before"], "destination_before": wallet["destination_before"],
                           "after": source_final, "destination_after": destination_final})
        report.update({"raw_send_result": raw_send_result, "after_raw_source": after_raw,
                       "single_send_result": single_result, "after_single_source": after_single,
                       "batch_result": batch, "accounts": finals,
                       "accepted_parents": 4, "accepted_logical_transfers": 4 * count,
                       "rejected_invalid_signature_parents": 1,
                       "independent_native_cell_hash_checks": 8,
                       "all_gates_passed": True})
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--wallet-dir", type=Path, required=True)
    parser.add_argument("--source-index", type=int, default=0)
    parser.add_argument("--lane-depth", type=int, choices=(0, 1, 2), default=2)
    parser.add_argument("--logical-count", type=int, default=16)
    parser.add_argument("--amount", type=int, default=1)
    parser.add_argument("--fee", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--canonical-timeout", type=float, default=180)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    require(args.source_index >= 0 and 1 <= args.logical_count <= 16, "Invalid wallet index or run count")
    require(0 < args.amount <= 2**64 - 1 and 0 <= args.fee <= 2**64 - 1 - args.amount, "Invalid amount/fee")
    require(0 < args.timeout <= 600 and 0 < args.canonical_timeout <= 600, "Invalid deadlines")
    require(not args.output.exists(), "Report already exists; choose a new output")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {"schema_version": 1, "test": "tonlib-native-json-liteserver-integration", "all_gates_passed": False,
              "started_unix_seconds": time.time(), "requests": [], "capacity_or_tps_claim": None}
    status = 1
    try:
        run(args, report)
        status = 0
    except Exception as error:
        report["error"] = {"type": type(error).__name__, "message": str(error)}
    finally:
        report["finished_unix_seconds"] = time.time()
        with args.output.open("x") as handle:
            json.dump(report, handle, indent=2, allow_nan=False)
            handle.write("\n")
    print(json.dumps({"all_gates_passed": report["all_gates_passed"], "report": str(args.output),
                      "error": report.get("error")}))
    return status


if __name__ == "__main__":
    sys.exit(main())
