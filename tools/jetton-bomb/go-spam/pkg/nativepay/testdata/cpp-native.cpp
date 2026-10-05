#include <iostream>
#include "tonlib/native-client.h"
#include "vm/boc.h"

std::string hex(td::Slice s) {
  static const char* digits="0123456789abcdef";
  std::string out;
  for (unsigned char c: s) { out += digits[c>>4]; out += digits[c&15]; }
  return out;
}
ton::Bits256 filled(char c) { ton::Bits256 x; x.as_slice().copy_from(std::string(32,c)); return x; }
int main() {
  td::Ed25519::PrivateKey key(td::SecureString(std::string(32,'\x42')));
  auto domain=filled('\x51');
  std::cout<<"[\n";
  for (int n : {0,1,3,16}) {
    std::vector<block::NativeTransferRunOutput> outputs;
    if (!n) outputs.push_back({filled('\x31'),123,7});
    else for (int i=0;i<n;i++) outputs.push_back({filled(static_cast<char>(i+1)),static_cast<td::uint64>(i+1),static_cast<td::uint64>(i)});
    auto msg=tonlib::native_client::create_message(key,outputs,19,0xffffffff,domain,n!=0).move_as_ok();
    std::string payload,sig;
    if (n) { auto parsed=block::NativeTransferRun::unpack_external(msg.root).move_as_ok(); payload=parsed.signing_payload(domain);sig=parsed.signature; }
    else { auto parsed=block::NativeTransfer::unpack_external(msg.root).move_as_ok(); payload=parsed.signing_payload(domain);sig=parsed.signature; }
    auto boc=vm::std_boc_serialize(msg.root).move_as_ok();
    std::cout<<"  {\"kind\":\""<<(n?"run":"scalar")<<"\",\"count\":"<<(n?n:1)<<",\"source\":\""<<msg.source.to_hex()<<"\",\"payload\":\""<<hex(payload)<<"\",\"signature\":\""<<hex(sig)<<"\",\"root_hash\":\""<<msg.hash.to_hex()<<"\",\"boc\":\""<<hex(boc.as_slice())<<"\"}"<<(n==16?"\n":",\n");
  }
  std::cout<<"]\n";
}
