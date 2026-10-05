# C++ native payment oracle

`cpp-native.json` was generated offline on 2026-10-05 by `cpp-native.cpp`, linked
against this repository's existing `tonlib::native_client::create_message` and
`block::NativeTransfer` / `NativeTransferRun` implementations. It contains the
source account, signed byte payload, Ed25519 signature, canonical cell hash and
BOC for a scalar payment and signed runs of 1, 3 and 16 outputs.

The fixture seed is 32 copies of `0x42`; the chain domain is 32 copies of `0x51`.
Nonce is 19 and expiry is `0xffffffff`. Scalar destination is 32 copies of
`0x31`, amount 123 and fee 7. Run output `i` (zero based) has destination filled
with byte `i+1`, amount `i+1` and fee `i`. These are public deterministic test
values, not funded wallet credentials.

To regenerate, build the repository's `test-tonlib-offline` target, then inspect
`ninja -C build -t commands test-tonlib-offline`. Reuse the compilation command
for `tonlib/test/test-native-client.cpp` with this helper as input, changing its
output object and removing dependency-output flags `-MD`, `-MT`, and `-MF`.
Reuse the target's final linker command, replacing its test object files with
the helper object and choosing a separate executable path. Run that executable
offline and retain its JSON stdout. No node or network transaction is involved.

`TestCppNativeClientParity` compares Go construction with the C++ payload,
signature, source and root hash, and independently parses the retained C++ BOC.
The BOC serialization byte order itself is not a protocol identity; its canonical
cell root hash is.
